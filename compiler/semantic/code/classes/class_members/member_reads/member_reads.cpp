#include "compiler/semantic/code/classes/class_members/member_reads/member_reads.h"
#include "compiler/program/library/events/emitter_member_table/emitter_member_table.h"
#include "compiler/program/library/fs/fs_class_shape/fs_class_shape.h"

namespace tz {

namespace {

constexpr const char *kMemberPlaceholder = "x";

}

MemberReads::MemberReads(SemanticState &owner, ClassFacts &sharedFacts,
                         ClassMemberRules &sharedClassMemberRules)
    : facts(sharedFacts),
      state(owner),
      classMemberRules(sharedClassMemberRules) {}

void MemberReads::classValueType(const ClassValueType &task) {
  const Type *type = this->facts.valueTypeOf(task.classDecl);

  return this->state.walk.tasks.pushAnswer(type);
}

void MemberReads::checkClassValue(const CheckClassValue &task) {
  NameExpr *name = this->classMemberRules.classValueName(task.name, task.classDecl);

  return this->state.walk.tasks.pushChecked(name);
}

void MemberReads::checkThis(const CheckThis &task) {
  NameExpr *name = task.name;
  auto &tasks = this->state.walk.tasks;
  name->type = Type::error();
  tasks.pushChecked(name);
  const FunctionDecl *currentDecl = this->state.walk.functions.getCurrentDecl();
  auto found = this->checkThisOf(currentDecl);
  if (found.isErr()) {
    return this->state.diagnostics.error(name->loc(), found.error().message);
  }
  const FunctionDecl *source = found.value();
  VarSymbol *variable = source->takesThis() ? source->thisVar() : source->params[0].sym;
  auto declared = this->checkThisDeclared(source, variable);
  if (declared.isErr()) {
    return this->state.diagnostics.error(name->loc(), declared.error().message);
  }
  name->binding = NamedVar{.var = variable};
  name->type = variable->type;
  auto ready = this->checkReadyThis(variable);
  if (ready.isErr()) {
    bool isBeforeSuper = ready.error().code == CodeClassesRegistryReadyThisErrorCode::BEFORE_SUPER;
    if (isBeforeSuper) {
      name->type = Type::error();
    }
    this->state.diagnostics.error(name->loc(), ready.error().message);
    return tasks.pushTask(NoteUse{.variable = variable, .loc = name->loc()});
  }
  const Type *narrowed =
      source->takesThis() ? nullptr : this->state.walk.flow.findNarrowed(variable);
  if (narrowed != nullptr) {
    tasks.pushTask(Convert{.to = narrowed});
  }

  return tasks.pushTask(NoteUse{.variable = variable, .loc = name->loc()});
}

MemberReads::ResultThisDeclared MemberReads::checkThisDeclared(const FunctionDecl *source,
                                                               const VarSymbol *variable) const {
  if (variable == nullptr) {
    auto message = ErrorTextThis::undeclared(source->name);

    return err(CodeClassesRegistryCheckThisError{
        .code = CodeClassesRegistryCheckThisErrorCode::THIS_NOT_DECLARED,
        .message = message,
    });
  }

  return ok(variable);
}

void MemberReads::checkClassMember(const CheckClassMember &task) {
  MemberExpr *member = task.member;
  auto *classDecl = this->facts.classOf(member->object->type);
  auto &tasks = this->state.walk.tasks;
  member->type = Type::error();
  tasks.pushTask(NoteTlsMethod{.member = member, .classDecl = classDecl});
  bool isStatsDate = this->state.library.filesystem.isStatsClass(classDecl) &&
                     FsClassShape::isStatsDate(member->name);
  if (isStatsDate) {
    return tasks.pushTask(CheckStatsDate{.member = member, .classDecl = classDecl});
  }
  bool isViaThis = this->facts.isInitThis(member->object).isOk();
  const ClassDecl *owner = nullptr;
  const auto *field = classDecl->findField(member->name, &owner);
  if (field != nullptr && field->method() == nullptr) {
    return this->pushFieldMember(member, field, owner, isViaThis);
  }

  return this->startMethodMember(member, classDecl, task.expected, isViaThis);
}

void MemberReads::pushFieldMember(MemberExpr *member, const ClassField *field,
                                  const ClassDecl *owner, bool isViaThis) {
  llvm::StringRef name = member->name;
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(member);
  auto accessible = this->facts.checkAccess(owner, field->access, name, member->object);
  if (accessible.isErr()) {
    this->state.diagnostics.error(member->loc(), accessible.error().message);
  }
  auto set = this->classMemberRules.checkFieldSet(owner, name, isViaThis);
  if (set.isErr()) {
    return this->state.diagnostics.error(member->loc(), set.error().message);
  }
  auto typed = this->classMemberRules.checkFieldTyped(field, name);
  if (typed.isErr()) {
    return this->state.diagnostics.error(member->loc(), typed.error().message);
  }
  member->type = typed.value();
  const auto *narrowed = this->state.flow.tracked.narrowedFieldType(member);
  if (narrowed != nullptr) {
    return tasks.pushTask(Convert{.to = narrowed});
  }
  if (!field->resolved->isOptional()) {
    return;
  }
  auto path = this->state.flow.tracked.fieldPath(member);
  bool isDefined = path.has_value() && this->state.walk.flow.isFieldDefined(*path);
  if (isDefined) {
    return tasks.pushTask(Convert{.to = field->resolved->inner()});
  }
}

void MemberReads::startMethodMember(MemberExpr *member, ClassDecl *classDecl, const Type *expected,
                                    bool isViaThis) {
  llvm::StringRef name = member->name;
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(member);
  auto found = this->checkInstanceMethods(classDecl, name);
  if (found.isErr()) {
    return this->state.diagnostics.error(member->loc(), found.error().message);
  }
  std::vector<FunctionDecl *> methods = found.value();
  auto accessible =
      this->facts.checkAccess(methods[0]->cls(), methods[0]->access(), name, member->object);
  if (accessible.isErr()) {
    this->state.diagnostics.error(member->loc(), accessible.error().message);
  }
  bool isEmitterValue = methods[0]->cls()->library() &&
                        this->state.library.events.isEmitterClass(classDecl) &&
                        !this->state.library.declarations.isInsideLibrary(member->loc()) &&
                        !EmitterMemberTable::isName(name, EmitterMember::EVENT_NAMES);
  if (isEmitterValue) {
    return tasks.pushTask(ReportEmitterMethodValue{.member = member});
  }
  auto ready = this->checkObjectReady(isViaThis, name);
  if (ready.isErr()) {
    this->state.diagnostics.error(member->loc(), ready.error().message);
  }
  tasks.pushTask(AfterMethodVariants{
      .member = member,
      .classDecl = classDecl,
      .expected = expected,
      .methods = methods,
  });

  return this->classMemberRules.pushVariantTypes(methods, expected);
}

void MemberReads::afterMethodVariants(const AfterMethodVariants &task) {
  MemberExpr *member = task.member;
  auto types = this->classMemberRules.takeVariantTypes(task.methods, task.expected);
  auto variant = this->classMemberRules.variantForType(task.methods, types, task.expected, member);
  if (variant.isErr()) {
    return this->state.diagnostics.error(member->loc(), variant.error().message);
  }
  FuncSymbol *function = variant.value();
  auto plain = this->checkMethodValue(function, member->name);
  if (plain.isErr()) {
    return this->state.diagnostics.error(member->loc(), plain.error().message);
  }
  member->form = this->methodForm(task.classDecl, function);
  this->state.walk.tasks.pushTask(AfterMemberFunctionType{.member = member});

  return this->state.walk.tasks.pushTask(FunctionTypeOf{.function = function});
}

void MemberReads::afterMemberFunctionType(const AfterMemberFunctionType &task) {
  task.member->type = this->state.walk.tasks.takeAnswer();
}

void MemberReads::checkOverloadMember(const CheckOverloadMember &task) {
  MemberExpr *member = task.member;
  const Type *interfaceType = member->object->type;
  const Overload &overload = *interfaceType->overload(member->name);
  member->type = Type::error();
  auto variants = this->facts.signatureVariants(interfaceType, overload);
  this->state.walk.tasks.pushChecked(member);
  this->state.walk.tasks.pushTask(
      AfterOverloadVariants{.member = member, .expected = task.expected, .variants = variants});

  return this->classMemberRules.pushVariantTypes(variants, task.expected);
}

void MemberReads::afterOverloadVariants(const AfterOverloadVariants &task) {
  MemberExpr *member = task.member;
  const Type *interfaceType = member->object->type;
  const Overload &overload = *interfaceType->overload(member->name);
  auto types = this->classMemberRules.takeVariantTypes(task.variants, task.expected);
  auto variant = this->classMemberRules.variantForType(task.variants, types, task.expected, member);
  if (variant.isErr()) {
    return this->state.diagnostics.error(member->loc(), variant.error().message);
  }
  int signature = this->facts.signatureIndex(task.variants, variant.value());
  member->form = OverloadMember{.signature = signature};
  member->type = overload.signatures[size_t(signature)];
}

MemberReads::ResultThisComesFrom MemberReads::checkThisOf(const FunctionDecl *current) const {
  auto member = this->thisMemberText();
  const FunctionDecl *source = current != nullptr ? current->thisSource() : nullptr;
  const ClassDecl *currentClass = this->state.walk.self.getCurrentClass();
  bool isInStaticField =
      source == nullptr && this->state.walk.self.isInsideStaticInit() && currentClass != nullptr;
  if (isInStaticField) {
    auto message = ErrorTextThisOf::inField(currentClass->name, member);

    return err(CodeClassesRegistryThisOfError{
        .code = CodeClassesRegistryThisOfErrorCode::IN_STATIC_FIELD,
        .message = message,
    });
  }
  if (source == nullptr) {
    auto message = ErrorTextThisOf::outside();

    return err(CodeClassesRegistryThisOfError{
        .code = CodeClassesRegistryThisOfErrorCode::OUTSIDE_METHOD,
        .message = message,
    });
  }
  bool hasThis = source->takesThis() || source->hasThisParam();
  if (!hasThis && source->cls() != nullptr && source->isStatic()) {
    auto message = ErrorTextThisOf::inStatic(source->cls()->name, member);

    return err(CodeClassesRegistryThisOfError{
        .code = CodeClassesRegistryThisOfErrorCode::IN_STATIC_METHOD,
        .message = message,
    });
  }
  if (!hasThis) {
    auto message = ErrorTextThisOf::inFunction();

    return err(CodeClassesRegistryThisOfError{
        .code = CodeClassesRegistryThisOfErrorCode::IN_PLAIN_FUNCTION,
        .message = message,
    });
  }

  return ok(source);
}

std::string MemberReads::thisMemberText() const {
  llvm::StringRef member = this->state.walk.self.getThisMember();
  if (member.empty()) {
    return kMemberPlaceholder;
  }

  return member.str();
}

MemberReads::ResultThisReadiness MemberReads::checkReadyThis(VarSymbol *variable) const {
  const FunctionDecl *initializer = this->state.walk.self.getInitializer();
  const FunctionDecl *currentDecl = this->state.walk.functions.getCurrentDecl();
  bool isInInit = variable == this->state.walk.self.getInitializerThis();
  bool isInConstructor = isInInit && currentDecl == initializer;
  bool isInClosure = isInInit && currentDecl != initializer;
  if (isInClosure && !this->state.walk.self.isClosureThisReady()) {
    auto message = ErrorTextReadyThis::inClosure();

    return err(CodeClassesRegistryReadyThisError{
        .code = CodeClassesRegistryReadyThisErrorCode::IN_CLOSURE_BEFORE_FIELDS_SET,
        .message = message,
    });
  }
  if (isInConstructor && this->state.walk.flow.isBeforeSuper()) {
    auto message = ErrorTextReadyThis::noSuper(initializer->cls()->parent->name);

    return err(CodeClassesRegistryReadyThisError{
        .code = CodeClassesRegistryReadyThisErrorCode::BEFORE_SUPER,
        .message = message,
    });
  }
  bool hasFieldsMatter = isInConstructor && this->state.walk.self.getThisMember().empty();
  const Flow &flow = this->state.walk.flow.getFlow();
  auto unset = this->facts.firstUnset(flow);
  if (hasFieldsMatter && unset.isOk()) {
    auto message = ErrorTextReadyThis::unset(unset.value());

    return err(CodeClassesRegistryReadyThisError{
        .code = CodeClassesRegistryReadyThisErrorCode::BEFORE_FIELDS_SET,
        .message = message,
    });
  }

  return ok(variable);
}

MemberReads::ResultMethodsOfObjects MemberReads::checkInstanceMethods(const ClassDecl *classDecl,
                                                                      llvm::StringRef name) const {
  auto methods = classDecl->findMethodsNamed(name);
  auto staticMethods = classDecl->findStaticMethodsNamed(name);
  bool isStaticMember = !staticMethods.empty() || this->hasStaticField(classDecl, name);
  if (methods.empty() && isStaticMember) {
    auto message = ErrorTextClassField::isStatic(name, classDecl->name);

    return err(CodeClassesRegistryInstanceMethodsError{
        .code = CodeClassesRegistryInstanceMethodsErrorCode::STATIC_MEMBER,
        .message = message,
    });
  }
  if (methods.empty()) {
    auto message = ErrorTextClassField::noProperty(name, classDecl->type->name());

    return err(CodeClassesRegistryInstanceMethodsError{
        .code = CodeClassesRegistryInstanceMethodsErrorCode::NO_SUCH_PROPERTY,
        .message = message,
    });
  }

  return ok(methods);
}

bool MemberReads::hasStaticField(const ClassDecl *classDecl, llvm::StringRef name) const {
  for (const ClassField &field : classDecl->fields) {
    if (field.isStatic() && field.name == name) {
      return true;
    }
  }

  return false;
}

MemberReads::ResultObjectReadyToUse MemberReads::checkObjectReady(bool isViaThis,
                                                                  llvm::StringRef name) const {
  const Flow &flow = this->state.walk.flow.getFlow();
  bool isEarly = isViaThis && !this->state.walk.flow.isBeforeSuper() &&
                 this->facts.checkThisReady(flow).isErr();
  if (isEarly) {
    auto unset = this->facts.firstUnset(flow);
    auto message = ErrorTextClassField::earlyThis(name, unset.value());

    return err(CodeClassesRegistryObjectReadyError{
        .code = CodeClassesRegistryObjectReadyErrorCode::USED_BEFORE_SET,
        .message = message,
    });
  }

  return ok(name);
}

MemberReads::ResultPlainMethodValue MemberReads::checkMethodValue(const FuncSymbol *function,
                                                                  llvm::StringRef name) const {
  if (function->isGeneric()) {
    auto message = ErrorTextClassField::genericVal(name);

    return err(CodeClassesRegistryMethodValueError{
        .code = CodeClassesRegistryMethodValueErrorCode::GENERIC_METHOD,
        .message = message,
    });
  }

  return ok(function);
}

MethodMember MemberReads::methodForm(ClassDecl *classDecl, FuncSymbol *function) const {
  auto *implementation = this->facts.soleImplementation(classDecl, function->decl);
  if (implementation != nullptr) {
    return MethodMember{.method = implementation->sym};
  }

  return MethodMember{.method = function, .virtualCall = true};
}

}
