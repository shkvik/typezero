#include "compiler/semantic/code/classes/class_bodies/class_body_walks/class_body_walks.h"
#include "compiler/program/library/builtin_names/keyword_table/keyword_table.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

ClassBodyWalks::ClassBodyWalks(SemanticState &owner, ClassFacts &sharedFacts)
    : facts(sharedFacts),
      state(owner) {}

void ClassBodyWalks::walkClassBodies(const WalkClassBodies &task) {
  ClassDecl *classDecl = task.classDecl;
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(FinishClassBodies{
      .classDecl = classDecl,
      .returns = KnownReturns{.known = {}, .missing = {}},
  });
  for (FunctionDecl *functionDecl : llvm::reverse(this->classMethods(classDecl))) {
    tasks.pushTask(WalkClassMethod{.functionDecl = functionDecl});
  }
  for (FunctionDecl *constructor : llvm::reverse(classDecl->constructors)) {
    tasks.pushTask(WalkClassFunction{.functionDecl = constructor});
  }
  if (classDecl->constructors.empty()) {
    tasks.pushTask(WalkFieldStarts{.classDecl = classDecl});
  }
  if (classDecl->init != nullptr) {
    return tasks.pushTask(WalkClassFunction{.functionDecl = classDecl->init});
  }
}

void ClassBodyWalks::walkClassFunction(const WalkClassFunction &task) {
  if (!task.functionDecl->checked) {
    return this->state.walk.tasks.pushTask(
        WalkFunctionFromScratch{.functionDecl = task.functionDecl});
  }
}

void ClassBodyWalks::walkFieldStarts(const WalkFieldStarts &task) {
  const ClassDecl *classDecl = task.classDecl;
  for (const ClassField &field : classDecl->fields) {
    auto started = this->checkFieldStartValue(classDecl, field);
    if (started.isErr()) {
      this->state.diagnostics.error(field.loc, started.error().message);
    }
  }
}

void ClassBodyWalks::walkClassMethod(const WalkClassMethod &task) {
  FunctionDecl *functionDecl = task.functionDecl;
  if (this->state.code.late.isTemplate(functionDecl->sym)) {
    return;
  }
  this->state.walk.tasks.pushTask(AfterClassMethodReturn{.functionDecl = functionDecl});
  if (!functionDecl->sym->hasReturnType()) {
    return this->state.walk.tasks.pushTask(InferReturnType{.function = functionDecl->sym});
  }
}

void ClassBodyWalks::afterClassMethodReturn(const AfterClassMethodReturn &task) {
  FunctionDecl *functionDecl = task.functionDecl;
  if (functionDecl->checked || functionDecl->isAbstract()) {
    return;
  }

  return this->state.walk.tasks.pushTask(WalkFunctionFromScratch{.functionDecl = functionDecl});
}

void ClassBodyWalks::finishClassBodies(const FinishClassBodies &task) {
  ClassDecl *classDecl = task.classDecl;
  auto returns = task.returns;
  auto &tasks = this->state.walk.tasks;
  auto answers = tasks.takeAnswers(returns.missing.size());
  for (size_t index = 0; index < answers.size(); ++index) {
    returns.known.push_back({returns.missing[index], answers[index]});
  }
  returns.missing.clear();
  this->state.diagnostics.pushList(DiagnosticRoute::DISCARD);
  this->walkImplements(classDecl, returns);
  this->walkOverrideReturns(classDecl, returns);
  this->state.diagnostics.popList();
  if (returns.missing.empty()) {
    this->walkAbstract(classDecl);
    this->walkImplements(classDecl, returns);

    return this->walkOverrideReturns(classDecl, returns);
  }
  auto missing = returns.missing;
  tasks.pushTask(FinishClassBodies{.classDecl = classDecl, .returns = std::move(returns)});
  for (FuncSymbol *function : llvm::reverse(missing)) {
    tasks.pushTask(ReturnTypeOf{.function = function});
  }
}

const Type *ClassBodyWalks::knownReturn(FuncSymbol *function, KnownReturns &returns) const {
  for (const auto &[known, type] : returns.known) {
    if (known == function) {
      return type;
    }
  }
  if (!llvm::is_contained(returns.missing, function)) {
    returns.missing.push_back(function);
  }

  return Type::error();
}

void ClassBodyWalks::bindOuterTypeParams(const BindOuterTypeParams &task) {
  const FunctionDecl *outer = task.classDecl != nullptr ? task.classDecl->outer : nullptr;
  if (outer != nullptr) {
    return this->state.walk.tasks.pushTask(BindFunctionTypeParams{.functionDecl = outer});
  }
}

void ClassBodyWalks::declareThis(const DeclareThis &task) {
  FunctionDecl *functionDecl = task.functionDecl;
  auto declared = this->checkMethodTakesThis(functionDecl);
  if (declared.isErr()) {
    return this->state.diagnostics.error(functionDecl->loc(), declared.error().message);
  }
  auto *self = this->state.ast.newVar();
  self->name = KeywordTable::getName(Keyword::THIS);
  self->loc = functionDecl->loc();
  self->type = functionDecl->cls()->type;
  self->isConst = true;
  unsigned declSeq = this->state.declaration.order.takeSequenceNumber();
  self->place = FunctionLocalVar{.owner = functionDecl, .state = {.declSeq = declSeq}};
  ClassMember *member = functionDecl->member();
  switch (member->method.kind()) {
    case MethodKind::INSTANCE_METHOD:
      member->method.as<InstanceMethod>()->thisVar = self;
      return;
    case MethodKind::STATIC_METHOD:
      return;
    case MethodKind::CONSTRUCTOR:
      member->method.as<Constructor>()->thisVar = self;
      return;
    case MethodKind::ABSTRACT_METHOD:
      member->method.as<AbstractMethod>()->thisVar = self;
      return;
  }
}

ClassBodyWalks::ResultMethodTakesThis
ClassBodyWalks::checkMethodTakesThis(const FunctionDecl *functionDecl) const {
  if (!functionDecl->takesThis()) {
    auto message = ErrorTextDeclareThis::notMethod(functionDecl->name);

    return err(CodeClassesRegistryDeclareThisError{
        .code = CodeClassesRegistryDeclareThisErrorCode::NOT_A_METHOD,
        .message = message,
    });
  }

  return ok(functionDecl);
}

void ClassBodyWalks::checkFieldsSet(const CheckFieldsSet &task) {
  auto fieldsSet = this->checkEveryFieldSet();
  if (fieldsSet.isErr()) {
    return this->state.diagnostics.error(task.loc, fieldsSet.error().message);
  }
}

ClassBodyWalks::ResultEveryFieldSet ClassBodyWalks::checkEveryFieldSet() const {
  const Flow &flow = this->state.walk.flow.getFlow();
  auto unset = this->facts.firstUnset(flow);
  const FunctionDecl *initializer = this->state.walk.self.getInitializer();
  if (!this->state.walk.flow.isUnreachable() && unset.isOk()) {
    auto message = ErrorTextFieldsSet::pathUnset(initializer->cls()->name, unset.value());

    return err(CodeClassesRegistryCheckFieldsSetError{
        .code = CodeClassesRegistryCheckFieldsSetErrorCode::FIELD_NOT_SET_ON_EVERY_PATH,
        .message = message,
    });
  }

  return ok(initializer);
}

ClassBodyWalks::ResultFieldHasValue
ClassBodyWalks::checkFieldStartValue(const ClassDecl *classDecl, const ClassField &field) const {
  bool isNeedingValue = !field.isStatic() && field.method() == nullptr && !field.optional() &&
                        field.init == nullptr && !field.definite() && field.resolved != nullptr &&
                        !field.resolved->isError();
  if (isNeedingValue && this->facts.constructorsOf(classDecl).empty()) {
    auto message = ErrorTextStartValue::noStart(field.name, field.resolved->name());

    return err(CodeClassesRegistryFieldStartValueError{
        .code = CodeClassesRegistryFieldStartValueErrorCode::NO_START_VALUE,
        .message = message,
    });
  }
  if (isNeedingValue) {
    auto message = ErrorTextStartValue::neverSet(field.name, field.resolved->name());

    return err(CodeClassesRegistryFieldStartValueError{
        .code = CodeClassesRegistryFieldStartValueErrorCode::NEVER_SET,
        .message = message,
    });
  }

  return ok(&field);
}

ClassBodyWalks::ResultReturnMatches ClassBodyWalks::checkOverrideReturn(const ClassDecl *classDecl,
                                                                        const FunctionDecl *method,
                                                                        KnownReturns &returns) {
  const FunctionDecl *overridden = method->overrides();
  const Type *mine = overridden != nullptr ? this->knownReturn(method->sym, returns) : nullptr;
  const Type *theirs =
      overridden != nullptr ? this->knownReturn(overridden->sym, returns) : nullptr;

  bool isKnown = mine != nullptr && theirs != nullptr && !mine->isError() && !theirs->isError();
  bool isNarrowerObject = isKnown && mine->nonOptional()->isObject() &&
                          theirs->nonOptional()->isObject() &&
                          this->state.types.relations.isImplicitlyConvertible(mine, theirs);

  if (isKnown && mine != theirs && !isNarrowerObject) {
    auto message = ErrorTextOverride::otherType(method->name, mine->name(), classDecl->name,
                                                theirs->name(), overridden->cls()->name);

    return err(CodeClassesRegistryOverrideReturnError{
        .code = CodeClassesRegistryOverrideReturnErrorCode::RETURNS_OTHER_TYPE,
        .message = message,
    });
  }

  return ok(method);
}

std::vector<FunctionDecl *> ClassBodyWalks::classMethods(const ClassDecl *classDecl) const {
  std::vector<FunctionDecl *> methods = classDecl->methods;
  for (const ClassField &field : classDecl->fields) {
    if (field.method() != nullptr) {
      methods.push_back(field.method());
    }
  }

  return methods;
}

void ClassBodyWalks::walkOverrideReturns(const ClassDecl *classDecl, KnownReturns &returns) {
  for (FunctionDecl *functionDecl : this->facts.ownMethods(classDecl)) {
    auto returned = this->checkOverrideReturn(classDecl, functionDecl, returns);
    if (returned.isErr()) {
      this->state.diagnostics.error(functionDecl->loc(), returned.error().message);
    }
  }
}

bool ClassBodyWalks::isTakingParams(const Type *functionType, const FuncSymbol *function) const {
  return functionType->isFunction() &&
         functionType->params() == llvm::ArrayRef<const Type *>(function->params) &&
         functionType->minArgs() == function->minArgs &&
         functionType->hasRest() == function->hasRest;
}

void ClassBodyWalks::interfacesOf(const ClassDecl *classDecl,
                                  std::vector<const Type *> &out) const {
  std::vector<const Type *> queue(classDecl->implemented.begin(), classDecl->implemented.end());
  for (size_t index = 0; index < queue.size(); ++index) {
    const Type *next = queue[index];
    if (!llvm::is_contained(out, next)) {
      this->addInterface(next, queue, out);
    }
  }
}

void ClassBodyWalks::addInterface(const Type *interfaceType, std::vector<const Type *> &queue,
                                  std::vector<const Type *> &out) const {
  out.push_back(interfaceType);
  for (const Type *base : interfaceType->bases()) {
    queue.push_back(base);
  }
}

ClassBodyWalks::ResultAbstractHasClass
ClassBodyWalks::checkAbstractNeedsClass(const ClassDecl *classDecl,
                                        const FunctionDecl *functionDecl) const {
  if (functionDecl->isAbstract() && !classDecl->isAbstract) {
    auto message = ErrorTextAbstract::needsClass(functionDecl->name, classDecl->name);

    return err(CodeClassesRegistryAbstractNeedsClassError{
        .code = CodeClassesRegistryAbstractNeedsClassErrorCode::ABSTRACT_IN_CONCRETE,
        .message = message,
    });
  }

  return ok(functionDecl);
}

ClassBodyWalks::ResultAbstractOverride
ClassBodyWalks::checkAbstractImplemented(const ClassDecl *classDecl,
                                         const FunctionDecl *found) const {
  if (found->cls() != classDecl) {
    auto variant = ErrorTextVariant::text(found);
    auto message = ErrorTextAbstract::notImpl(classDecl->name, variant, found->cls()->name);

    return err(CodeClassesRegistryAbstractImplementedError{
        .code = CodeClassesRegistryAbstractImplementedErrorCode::NOT_IMPLEMENTED,
        .message = message,
    });
  }

  return ok(found);
}

void ClassBodyWalks::walkAbstract(ClassDecl *classDecl) {
  if (classDecl->literal() != nullptr) {
    return;
  }
  for (FunctionDecl *functionDecl : this->facts.ownMethods(classDecl)) {
    auto abstract = this->checkAbstractNeedsClass(classDecl, functionDecl);
    if (abstract.isErr()) {
      this->state.diagnostics.error(functionDecl->loc(), abstract.error().message);
    }
  }
  if (classDecl->isAbstract) {
    return;
  }
  for (size_t slot = 0; slot < classDecl->vtable.size(); ++slot) {
    if (classDecl->vtable[slot] == nullptr) {
      this->walkEmptySlot(classDecl, slot);
    }
  }
}

void ClassBodyWalks::walkEmptySlot(const ClassDecl *classDecl, size_t slot) {
  auto *found = this->slotMethod(classDecl, slot);
  if (found == nullptr) {
    return;
  }
  auto implemented = this->checkAbstractImplemented(classDecl, found);
  if (implemented.isErr()) {
    this->state.diagnostics.error(classDecl->loc(), implemented.error().message);
  }
}

FunctionDecl *ClassBodyWalks::slotMethod(const ClassDecl *classDecl, size_t slot) const {
  for (const ClassDecl *current = classDecl; current != nullptr; current = current->parent) {
    auto *found = this->ownSlotMethod(current, slot);
    if (found != nullptr) {
      return found;
    }
  }

  return nullptr;
}

FunctionDecl *ClassBodyWalks::ownSlotMethod(const ClassDecl *owner, size_t slot) const {
  auto methods = this->facts.ownMethods(owner);
  for (FunctionDecl *functionDecl : llvm::reverse(methods)) {
    if (functionDecl->slot() == int(slot)) {
      return functionDecl;
    }
  }

  return nullptr;
}

ClassBodyWalks::ResultFieldTypeMatches
ClassBodyWalks::checkFieldMatches(llvm::StringRef prefix, const Field &member, const Field *actual,
                                  const ClassDecl *classDecl) const {
  bool isSame =
      actual->type == member.type || WideningRelations::isSameReference(actual->type, member.type);
  if (!isSame) {
    auto message = ErrorTextImplements::fieldType(prefix, member.name, member.type->name(),
                                                  classDecl->name, actual->type->name());

    return err(CodeClassesRegistryFieldMatchesError{
        .code = CodeClassesRegistryFieldMatchesErrorCode::FIELD_TYPE_DIFFERS,
        .message = message,
    });
  }

  return ok(actual);
}

ClassBodyWalks::ResultMemberIsDeclared
ClassBodyWalks::checkMemberDeclared(llvm::StringRef prefix, const Field &member,
                                    llvm::ArrayRef<FunctionDecl *> methods) const {
  if (methods.empty()) {
    bool isOptional = member.type->isOptional();
    std::string type = isOptional ? member.type->inner()->name().str() : member.type->name().str();
    auto message = ErrorTextImplements::noMember(prefix, member.name, isOptional, type);

    return err(CodeClassesRegistryMemberDeclaredError{
        .code = CodeClassesRegistryMemberDeclaredErrorCode::NO_MEMBER,
        .message = message,
    });
  }

  return ok(&member);
}

ClassBodyWalks::ResultMemberIsFunction
ClassBodyWalks::checkMethodForField(const Field &member, const Type *interfaceType,
                                    const ClassDecl *classDecl) const {
  if (!member.type->isFunction()) {
    auto message = ErrorTextImplements::fieldNotFn(member.name, member.type->name(),
                                                   interfaceType->name(), classDecl->name);

    return err(CodeClassesRegistryMethodForFieldError{
        .code = CodeClassesRegistryMethodForFieldErrorCode::FIELD_AS_METHOD,
        .message = message,
    });
  }

  return ok(&member);
}

ClassBodyWalks::ResultReplaceableField
ClassBodyWalks::checkReplaceable(const Field &member, llvm::ArrayRef<FunctionDecl *> methods,
                                 const Type *interfaceType, const ClassDecl *classDecl) const {
  auto isArrow = std::bind_front(&ClassLinks::isArrowMethod, this->classLinks);
  if (!member.readonly && llvm::none_of(methods, isArrow)) {
    auto message =
        ErrorTextImplements::mustField(member.name, interfaceType->name(), classDecl->name);

    return err(CodeClassesRegistryReplaceableError{
        .code = CodeClassesRegistryReplaceableErrorCode::METHOD_NOT_FIELD,
        .message = message,
    });
  }

  return ok(&member);
}

ClassBodyWalks::ResultReturnTypeAgrees
ClassBodyWalks::checkReturnMatches(llvm::StringRef name, const Type *returnType,
                                   const Type *expected, const Type *interfaceType,
                                   const ClassDecl *classDecl) const {
  if (returnType != nullptr && !returnType->isError() && returnType != expected) {
    auto message = ErrorTextImplements::returns(name, returnType->name(), interfaceType->name(),
                                                classDecl->name, expected->name());

    return err(CodeClassesRegistryReturnMatchesError{
        .code = CodeClassesRegistryReturnMatchesErrorCode::RETURN_DIFFERS,
        .message = message,
    });
  }

  return ok(returnType);
}

ClassBodyWalks::ResultVariantForParams
ClassBodyWalks::checkVariantFound(llvm::StringRef prefix, const Field &member,
                                  llvm::ArrayRef<FunctionDecl *> methods,
                                  const ClassDecl *classDecl, bool isFound) const {
  if (!isFound) {
    auto variants = ErrorTextVariant::list(methods);
    auto message = ErrorTextImplements::params(prefix, member.name, member.type->name(),
                                               classDecl->name, variants);

    return err(CodeClassesRegistryVariantFoundError{
        .code = CodeClassesRegistryVariantFoundErrorCode::OTHER_PARAMETERS,
        .message = message,
    });
  }

  return ok(&member);
}

ClassBodyWalks::ResultSignatureVariant ClassBodyWalks::checkSignatureVariant(llvm::StringRef prefix,
                                                                             llvm::StringRef name,
                                                                             const Type *signature,
                                                                             bool isFound) const {
  if (!isFound) {
    auto message = ErrorTextImplements::noVariant(prefix, name, signature->name());

    return err(CodeClassesRegistrySignatureVariantError{
        .code = CodeClassesRegistrySignatureVariantErrorCode::NO_VARIANT,
        .message = message,
    });
  }

  return ok(signature);
}

void ClassBodyWalks::walkImplements(ClassDecl *classDecl, KnownReturns &returns) {
  auto interfaces = std::vector<const Type *>();
  this->interfacesOf(classDecl, interfaces);
  for (const Type *interfaceType : interfaces) {
    if (llvm::is_contained(classDecl->implemented, interfaceType)) {
      this->walkImplementedInterface(classDecl, interfaceType, returns);
    }
  }
}

void ClassBodyWalks::walkImplementedInterface(ClassDecl *classDecl, const Type *interfaceType,
                                              KnownReturns &returns) {
  auto prefix = ErrorTextImplements::prefix(classDecl->name, interfaceType->name());
  for (const Field &member : interfaceType->fields()) {
    this->walkImplementedMember(classDecl, interfaceType, member, prefix, returns);
  }
  for (const Overload &overload : interfaceType->overloads()) {
    this->walkImplementedOverload(classDecl, interfaceType, overload, prefix, returns);
  }
}

void ClassBodyWalks::walkImplementedMember(ClassDecl *classDecl, const Type *interfaceType,
                                           const Field &member, const std::string &prefix,
                                           KnownReturns &returns) {
  const auto *field = classDecl->findField(member.name);
  if (field != nullptr && field->method() == nullptr) {
    return this->walkImplementedField(classDecl, member, field, prefix);
  }
  auto methods = classDecl->findMethodsNamed(member.name);
  auto declared = this->checkMemberDeclared(prefix, member, methods);
  if (declared.isErr()) {
    this->state.diagnostics.error(classDecl->loc(), declared.error().message);
    return;
  }
  auto function = this->checkMethodForField(member, interfaceType, classDecl);
  if (function.isErr()) {
    this->state.diagnostics.error(methods[0]->loc(), function.error().message);
    return;
  }
  auto replaced = this->checkReplaceable(member, methods, interfaceType, classDecl);
  if (replaced.isErr()) {
    this->state.diagnostics.error(methods[0]->loc(), replaced.error().message);
    return;
  }
  this->walkVariantReturns(classDecl, interfaceType, member, methods, returns);
  bool isFound = this->hasMatchingVariant(member, methods);
  auto variant = this->checkVariantFound(prefix, member, methods, classDecl, isFound);
  if (variant.isErr()) {
    this->state.diagnostics.error(methods[0]->loc(), variant.error().message);
  }
}

void ClassBodyWalks::walkImplementedField(const ClassDecl *classDecl, const Field &member,
                                          const ClassField *field, const std::string &prefix) {
  const auto *actual = classDecl->type->field(member.name);
  if (actual == nullptr || actual->type->isError() || member.type->isError()) {
    return;
  }
  llvm::SMLoc loc = classDecl->field(member.name) != nullptr ? field->loc : classDecl->loc();
  auto matches = this->checkFieldMatches(prefix, member, actual, classDecl);
  if (matches.isErr()) {
    this->state.diagnostics.error(loc, matches.error().message);
  }
}

void ClassBodyWalks::walkVariantReturns(const ClassDecl *classDecl, const Type *interfaceType,
                                        const Field &member, llvm::ArrayRef<FunctionDecl *> methods,
                                        KnownReturns &returns) {
  for (FunctionDecl *functionDecl : methods) {
    if (this->isTakingMember(member, functionDecl)) {
      this->walkVariantReturn(classDecl, interfaceType, member, functionDecl, returns);
    }
  }
}

void ClassBodyWalks::walkVariantReturn(const ClassDecl *classDecl, const Type *interfaceType,
                                       const Field &member, const FunctionDecl *functionDecl,
                                       KnownReturns &returns) {
  const auto *returnType = this->knownReturn(functionDecl->sym, returns);
  auto agrees = this->checkReturnMatches(functionDecl->name, returnType, member.type->ret(),
                                         interfaceType, classDecl);
  if (agrees.isErr()) {
    this->state.diagnostics.error(functionDecl->loc(), agrees.error().message);
  }
}

bool ClassBodyWalks::hasMatchingVariant(const Field &member,
                                        llvm::ArrayRef<FunctionDecl *> methods) const {
  for (const FunctionDecl *functionDecl : methods) {
    if (this->isTakingMember(member, functionDecl)) {
      return true;
    }
  }

  return false;
}

bool ClassBodyWalks::isTakingMember(const Field &member, const FunctionDecl *functionDecl) const {
  return functionDecl->sym != nullptr && this->isTakingParams(member.type, functionDecl->sym);
}

void ClassBodyWalks::walkImplementedOverload(ClassDecl *classDecl, const Type *interfaceType,
                                             const Overload &overload, const std::string &prefix,
                                             KnownReturns &returns) {
  auto methods = classDecl->findMethodsNamed(overload.name);
  for (const Type *signature : overload.signatures) {
    this->walkOverloadSignature(classDecl, interfaceType, overload, methods, signature, prefix,
                                returns);
  }
}

void ClassBodyWalks::walkOverloadSignature(ClassDecl *classDecl, const Type *interfaceType,
                                           const Overload &overload,
                                           llvm::ArrayRef<FunctionDecl *> methods,
                                           const Type *signature, const std::string &prefix,
                                           KnownReturns &returns) {
  auto *functionDecl = this->methodTaking(methods, signature);
  auto variant =
      this->checkSignatureVariant(prefix, overload.name, signature, functionDecl != nullptr);
  if (variant.isErr()) {
    this->state.diagnostics.error(classDecl->loc(), variant.error().message);
    return;
  }
  const auto *returnType = this->knownReturn(functionDecl->sym, returns);
  auto agrees = this->checkReturnMatches(overload.name, returnType, signature->ret(), interfaceType,
                                         classDecl);
  if (agrees.isErr()) {
    this->state.diagnostics.error(functionDecl->loc(), agrees.error().message);
  }
}

FunctionDecl *ClassBodyWalks::methodTaking(llvm::ArrayRef<FunctionDecl *> methods,
                                           const Type *signature) const {
  for (FunctionDecl *method : methods) {
    if (method->sym != nullptr && this->isTakingParams(signature, method->sym)) {
      return method;
    }
  }

  return nullptr;
}

}
