#include "compiler/semantic/code/classes/class_members/static_member_reads/static_member_reads.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"

using llvm::cast;
using llvm::isa;

namespace tz {

StaticMemberReads::StaticMemberReads(SemanticState &owner, ClassFacts &sharedFacts,
                                     ClassMemberRules &sharedClassMemberRules)
    : facts(sharedFacts),
      state(owner),
      classMemberRules(sharedClassMemberRules) {}

void StaticMemberReads::checkStaticMember(const CheckStaticMember &task) {
  MemberExpr *member = task.member;
  ClassDecl *classDecl = task.classDecl;
  member->type = Type::error();
  auto found = this->classMemberRules.staticField(classDecl, member->name);
  if (found.field != nullptr) {
    return this->pushStaticFieldRead(member, classDecl, *found.field, found.owner);
  }

  return this->startStaticMethodValue(member, classDecl, task.expected);
}

void StaticMemberReads::pushStaticFieldRead(MemberExpr *member, ClassDecl *classDecl,
                                            const ClassField &field, const ClassDecl *owner) {
  llvm::StringRef name = member->name;
  auto &tasks = this->state.walk.tasks;
  auto accessible = this->facts.checkAccess(owner, field.access, name);
  if (accessible.isErr()) {
    this->state.diagnostics.error(member->loc(), accessible.error().message);
  }
  auto *read = this->state.ast.make<NameExpr>(member->loc(), field.var()->name);
  read->binding = NamedVar{.var = field.var()};
  if (member->object != nullptr && member->object->loc() != member->loc()) {
    this->state.inventory.namedUses[read] = {classDecl, member->object->loc(), ""};
  }
  tasks.pushChecked(read);
  auto typed = this->checkStaticFieldTyped(owner, field.var(), name);
  if (typed.isErr()) {
    this->state.diagnostics.error(member->loc(), typed.error().message);
    read->type = Type::error();
    return;
  }
  auto ready = this->checkFieldReady(owner, field.var());
  if (ready.isErr()) {
    this->state.diagnostics.error(member->loc(), ready.error().message);
  }
  read->type = typed.value();
  if (read->type->isOptional() && this->state.walk.flow.isDefined(field.var())) {
    return tasks.pushTask(Convert{.to = read->type->inner()});
  }
}

void StaticMemberReads::startStaticMethodValue(MemberExpr *member, ClassDecl *classDecl,
                                               const Type *expected) {
  llvm::StringRef name = member->name;
  auto found = this->checkStaticMethods(classDecl, name);
  bool isClassProperty = BuiltinPropertyTable::isName(name, BuiltinProperty::NAME) ||
                         BuiltinPropertyTable::isName(name, BuiltinProperty::PROTOTYPE);
  if (found.isErr() && isClassProperty) {
    auto *object = cast<NameExpr>(member->object);
    member->object = this->classMemberRules.classValueName(object, classDecl);
    return this->checkClassRefMember(CheckClassRefMember{.member = member});
  }
  if (found.isErr()) {
    this->state.diagnostics.error(member->loc(), found.error().message);
    return this->state.walk.tasks.pushChecked(member);
  }
  std::vector<FunctionDecl *> methods = found.value();
  auto accessible = this->facts.checkAccess(methods[0]->cls(), methods[0]->access(), name);
  if (accessible.isErr()) {
    this->state.diagnostics.error(member->loc(), accessible.error().message);
  }
  this->state.walk.tasks.pushTask(
      AfterStaticVariants{.member = member, .expected = expected, .methods = methods});

  return this->classMemberRules.pushVariantTypes(methods, expected);
}

void StaticMemberReads::afterStaticVariants(const AfterStaticVariants &task) {
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  const Type *lastReturn = this->classMemberRules.takeLastVariantReturn(task.methods);
  auto types = this->classMemberRules.takeVariantTypes(task.methods, task.expected);
  auto variant =
      this->classMemberRules.variantForType(task.methods, types, lastReturn, task.expected, member);
  if (variant.isErr()) {
    this->state.diagnostics.error(member->loc(), variant.error().message);
    return tasks.pushChecked(member);
  }
  FuncSymbol *function = variant.value();
  auto *value = this->state.ast.make<NameExpr>(member->loc(), member->name);
  value->binding = NamedFunction{.func = function};
  tasks.pushChecked(value);
  tasks.pushTask(AfterNameFunctionType{.name = value});

  return tasks.pushTask(FunctionTypeOf{.function = function});
}

void StaticMemberReads::afterNameFunctionType(const AfterNameFunctionType &task) {
  task.name->type = this->state.walk.tasks.takeAnswer();
}

void StaticMemberReads::checkClassRefMember(const CheckClassRefMember &task) {
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  const Type *type = member->object->type;
  const ClassDecl *classDecl = type->classRef();
  member->type = Type::error();
  auto readable = this->checkClassRefReadable(member, classDecl);
  if (readable.isErr()) {
    this->state.diagnostics.error(member->loc(), readable.error().message);
    return tasks.pushChecked(member);
  }
  bool isName = BuiltinPropertyTable::isName(member->name, BuiltinProperty::NAME);
  if (classDecl == nullptr) {
    auto *call =
        this->state.ast.make<CallExpr>(member->loc(), member, std::vector<Expr *>{member->object});
    call->target = BuiltinCall{.builtin = Builtin::ClassName};
    call->type = Type::string();
    return tasks.pushChecked(call);
  }
  if (isName && !this->hasOwnNameMethod(classDecl)) {
    auto *text = this->classNameText(member, classDecl);
    return tasks.pushChecked(text);
  }
  auto *owner = const_cast<ClassDecl *>(classDecl);
  tasks.pushTask(AfterClassRefStatic{.object = member->object});

  return tasks.pushTask(
      CheckStaticMember{.member = member, .classDecl = owner, .expected = nullptr});
}

StaticMemberReads::ResultClassRefReadable
StaticMemberReads::checkClassRefReadable(const MemberExpr *member,
                                         const ClassDecl *classDecl) const {
  const Type *type = member->object->type;
  if (BuiltinPropertyTable::isName(member->name, BuiltinProperty::PROTOTYPE)) {
    auto message = ErrorTextClassRefMember::prototype();

    return err(CodeClassesRegistryCheckClassRefMemberError{
        .code = CodeClassesRegistryCheckClassRefMemberErrorCode::NO_PROTOTYPES,
        .message = message,
    });
  }
  bool isName = BuiltinPropertyTable::isName(member->name, BuiltinProperty::NAME);
  if (classDecl == nullptr && !isName) {
    auto message = ErrorTextClassRefMember::runtime(type->name());

    return err(CodeClassesRegistryCheckClassRefMemberError{
        .code = CodeClassesRegistryCheckClassRefMemberErrorCode::RUNTIME_CLASS,
        .message = message,
    });
  }
  bool isOwnName = classDecl != nullptr && isName && !this->hasOwnNameMethod(classDecl);
  if (classDecl != nullptr && !isOwnName && classDecl->type == nullptr) {
    auto message = ErrorTextClassRefMember::nameOnly(classDecl->name);

    return err(CodeClassesRegistryCheckClassRefMemberError{
        .code = CodeClassesRegistryCheckClassRefMemberErrorCode::VALUE_ONLY_CLASS,
        .message = message,
    });
  }

  return ok(member);
}

void StaticMemberReads::afterClassRefStatic(const AfterClassRefStatic &task) {
  Expr *read = this->state.walk.tasks.takeChecked();
  Expr *value = this->afterClassValue(task.object, read);

  return this->state.walk.tasks.pushChecked(value);
}

Expr *StaticMemberReads::afterClassValue(Expr *object, Expr *result) {
  if (isa<NameExpr>(object)) {
    return result;
  }
  auto *sequence =
      this->state.ast.make<SequenceExpr>(result->loc(), std::vector<Expr *>{object, result});
  sequence->type = result->type;

  return sequence;
}

bool StaticMemberReads::hasOwnNameMethod(const ClassDecl *classDecl) const {
  if (classDecl->type == nullptr) {
    return false;
  }
  auto nameName = BuiltinPropertyTable::getName(BuiltinProperty::NAME);
  auto methods = classDecl->findStaticMethodsNamed(nameName);

  return !methods.empty();
}

Expr *StaticMemberReads::classNameText(MemberExpr *member, const ClassDecl *classDecl) {
  auto text = std::u16string(classDecl->name.begin(), classDecl->name.end());
  auto *literal = this->state.ast.make<StringLitExpr>(member->loc(), text);
  literal->type = Type::string();

  return this->afterClassValue(member->object, literal);
}

StaticMemberReads::ResultStaticFieldType
StaticMemberReads::checkStaticFieldTyped(const ClassDecl *owner, const VarSymbol *variable,
                                         llvm::StringRef name) const {
  bool isEarly = !owner->declared &&
                 this->state.declaration.modules.getLoadingModule() != nullptr &&
                 !this->state.walk.functions.isInsideFunction();
  if (variable->type == nullptr && isEarly) {
    auto message = ErrorTextStaticOrder::usedEarly(variable->name, owner->name);

    return err(CodeClassesRegistryStaticFieldTypedError{
        .code = CodeClassesRegistryStaticFieldTypedErrorCode::USED_BEFORE_DECLARATION,
        .message = message,
    });
  }
  if (variable->type == nullptr) {
    auto message = ErrorTextStaticLater::typeOf(variable->name, name);

    return err(CodeClassesRegistryStaticFieldTypedError{
        .code = CodeClassesRegistryStaticFieldTypedErrorCode::TYPE_FROM_LATER_VALUE,
        .message = message,
    });
  }

  return ok(variable->type);
}

StaticMemberReads::ResultFieldValueIsSet
StaticMemberReads::checkFieldReady(const ClassDecl *owner, const VarSymbol *variable) const {
  bool isEarly = !owner->declared &&
                 this->state.declaration.modules.getLoadingModule() != nullptr &&
                 !this->state.walk.functions.isInsideFunction() &&
                 this->state.declaration.modules.findDeclModule(owner) ==
                     this->state.declaration.modules.getLoadingModule();
  if (isEarly) {
    auto message = ErrorTextStaticOrder::usedEarly(variable->name, owner->name);

    return err(CodeClassesRegistryFieldReadyError{
        .code = CodeClassesRegistryFieldReadyErrorCode::USED_BEFORE_DECLARATION,
        .message = message,
    });
  }
  bool isUnset = this->state.walk.self.isInsideStaticInit() &&
                 this->state.walk.self.getCurrentClass() == owner &&
                 !this->state.walk.self.isStaticSet(variable);
  if (isUnset) {
    auto message = ErrorTextStaticOrder::beforeSet(variable->name);

    return err(CodeClassesRegistryFieldReadyError{
        .code = CodeClassesRegistryFieldReadyErrorCode::READ_BEFORE_SET,
        .message = message,
    });
  }

  return ok(variable);
}

StaticMemberReads::ResultStaticFunctions
StaticMemberReads::checkStaticMethods(const ClassDecl *classDecl, llvm::StringRef name) const {
  auto methods = classDecl->findStaticMethodsNamed(name);
  bool isInstance = methods.empty() && (classDecl->findField(name) != nullptr ||
                                        !classDecl->findMethodsNamed(name).empty());
  if (isInstance) {
    auto message = ErrorTextStaticMember::instance(name, classDecl->name);

    return err(CodeClassesRegistryStaticMethodsError{
        .code = CodeClassesRegistryStaticMethodsErrorCode::INSTANCE_MEMBER,
        .message = message,
    });
  }
  if (methods.empty()) {
    auto message = ErrorTextStaticMember::noMember(classDecl->name, name);

    return err(CodeClassesRegistryStaticMethodsError{
        .code = CodeClassesRegistryStaticMethodsErrorCode::NO_STATIC_MEMBER,
        .message = message,
    });
  }

  return ok(methods);
}

}
