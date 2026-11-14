#include "compiler/semantic/code/classes/class_members/member_targets/member_targets.h"
#include "compiler/program/library/builtin_names/library_method_table/library_method_table.h"
#include "compiler/program/library/fs/fs_class_shape/fs_class_shape.h"

using llvm::cast;

namespace tz {

MemberTargets::MemberTargets(SemanticState &owner, ClassFacts &sharedFacts,
                             ClassMemberRules &sharedClassMemberRules)
    : facts(sharedFacts),
      state(owner),
      classMemberRules(sharedClassMemberRules) {}

void MemberTargets::checkFieldTarget(const CheckFieldTarget &task) {
  MemberExpr *member = task.member;
  const Type *type = member->object->type;
  auto *classDecl = this->facts.classOf(type);
  if (classDecl == nullptr) {
    return this->state.walk.tasks.pushTask(CheckObjectFieldTarget{.member = member});
  }
  const Type *target = this->fieldTargetType(member, classDecl, task.form);

  return this->state.walk.tasks.pushAnswer(target);
}

const Type *MemberTargets::fieldTargetType(MemberExpr *member, ClassDecl *classDecl,
                                           AssignForm form) {
  const Type *type = member->object->type;
  llvm::StringRef name = member->name;
  const ClassDecl *owner = nullptr;
  const auto *field = classDecl->findField(name, &owner);
  auto stored = this->checkStatsDate(classDecl, member);
  if (stored.isErr()) {
    this->state.diagnostics.error(member->loc(), stored.error().message);
    return Type::error();
  }
  auto assignable = this->checkAssignableField(field, classDecl, name, type);
  if (assignable.isErr()) {
    this->state.diagnostics.error(member->loc(), assignable.error().message);
    return Type::error();
  }
  auto accessible = this->facts.checkAccess(owner, field->access, name, member->object);
  if (accessible.isErr()) {
    this->state.diagnostics.error(member->loc(), accessible.error().message);
  }
  bool isViaThis = this->facts.isInitThis(member->object).isOk();
  bool isCompound = form == AssignForm::COMPOUND;
  bool isUnsetCompound = isCompound && this->reportUnsetField(member, owner, isViaThis);
  if (isUnsetCompound) {
    return Type::error();
  }
  auto writable = this->checkFieldWritable(field, owner, isViaThis, type);
  if (writable.isErr()) {
    this->state.diagnostics.error(member->loc(), writable.error().message);
  }
  bool isTypedLater = field->resolved == nullptr && isViaThis &&
                      this->state.walk.self.getInitializer() == classDecl->init &&
                      owner == classDecl && !isCompound;
  if (isTypedLater) {
    member->type = nullptr;
    return nullptr;
  }
  auto typed = this->classMemberRules.checkFieldTyped(field, name);
  if (typed.isErr()) {
    this->state.diagnostics.error(member->loc(), typed.error().message);
    return Type::error();
  }
  member->type = typed.value();

  return typed.value();
}

void MemberTargets::checkStaticTarget(const CheckStaticTarget &task) {
  const Type *target = this->staticTargetType(*task.target, task.classDecl);

  return this->state.walk.tasks.pushAnswer(target);
}

const Type *MemberTargets::staticTargetType(Expr *&target, ClassDecl *classDecl) {
  auto *member = cast<MemberExpr>(target);
  member->type = Type::error();
  member->object->type = Type::error();
  auto found = this->classMemberRules.staticField(classDecl, member->name);
  auto foundField = this->checkFieldFound(found.field, classDecl, member->name);
  if (foundField.isErr()) {
    this->state.diagnostics.error(member->loc(), foundField.error().message);
    return Type::error();
  }
  ClassField &field = *found.field;
  auto accessible = this->facts.checkAccess(found.owner, field.access, field.name);
  if (accessible.isErr()) {
    this->state.diagnostics.error(member->loc(), accessible.error().message);
  }
  VarSymbol *variable = field.var();
  auto *name = this->state.ast.make<NameExpr>(member->loc(), variable->name);
  name->binding = NamedVar{.var = variable};
  name->type = variable->type != nullptr ? variable->type : Type::error();
  target = name;
  auto writable = this->checkStaticFieldWritable(variable);
  if (writable.isErr()) {
    this->state.diagnostics.error(member->loc(), writable.error().message);
  }
  variable->reassigned = true;
  auto stated = this->checkTypeDeclared(variable, field.name);
  if (stated.isErr()) {
    this->state.diagnostics.error(member->loc(), stated.error().message);
    return Type::error();
  }

  return stated.value();
}

MemberTargets::ResultStoredFieldValue MemberTargets::checkStatsDate(ClassDecl *classDecl,
                                                                    MemberExpr *member) {
  bool isDerived = this->state.library.filesystem.isStatsClass(classDecl) &&
                   FsClassShape::isStatsDate(member->name);
  if (isDerived) {
    auto message = ErrorTextFieldTarget::statsDate(member->name);

    return err(CodeClassesRegistryStatsDateError{
        .code = CodeClassesRegistryStatsDateErrorCode::DERIVED_DATE,
        .message = message,
    });
  }

  return ok(classDecl);
}

MemberTargets::ResultAssignableMember
MemberTargets::checkAssignableField(const ClassField *field, const ClassDecl *classDecl,
                                    llvm::StringRef name, const Type *type) const {
  bool isAssignable = field != nullptr && field->method() == nullptr;
  if (isAssignable) {
    return ok(field);
  }
  auto setter = LibraryMethodTable::getSetterName(name);
  auto methods = classDecl->findMethodsNamed(name);
  bool isMethod = field != nullptr || !methods.empty();
  auto setters = classDecl->findMethodsNamed(setter);
  bool hasSetter = !setters.empty();
  if (isMethod && classDecl->library() && hasSetter) {
    auto message = ErrorTextFieldTarget::setter(name, type->name(), setter);

    return err(CodeClassesRegistryAssignableFieldError{
        .code = CodeClassesRegistryAssignableFieldErrorCode::SETTER_METHOD,
        .message = message,
    });
  }
  if (isMethod && classDecl->library()) {
    auto message = ErrorTextFieldTarget::library(name, type->name());

    return err(CodeClassesRegistryAssignableFieldError{
        .code = CodeClassesRegistryAssignableFieldErrorCode::LIBRARY_METHOD,
        .message = message,
    });
  }
  if (isMethod) {
    auto message = ErrorTextFieldTarget::method(name, type->name());

    return err(CodeClassesRegistryAssignableFieldError{
        .code = CodeClassesRegistryAssignableFieldErrorCode::CLASS_METHOD,
        .message = message,
    });
  }
  auto message = ErrorTextClassField::noProperty(name, type->name());

  return err(CodeClassesRegistryAssignableFieldError{
      .code = CodeClassesRegistryAssignableFieldErrorCode::NO_SUCH_PROPERTY,
      .message = message,
  });
}

MemberTargets::ResultFieldNotReadOnly MemberTargets::checkFieldWritable(const ClassField *field,
                                                                        const ClassDecl *owner,
                                                                        bool isViaThis,
                                                                        const Type *type) const {
  bool isInitializing = isViaThis && owner == this->state.walk.self.getInitializer()->cls();
  if (field->readonly && !isInitializing) {
    auto message = ErrorTextFieldTarget::readOnly(field->name, type->name());

    return err(CodeClassesRegistryFieldWritableError{
        .code = CodeClassesRegistryFieldWritableErrorCode::READ_ONLY,
        .message = message,
    });
  }

  return ok(field);
}

bool MemberTargets::reportUnsetField(const MemberExpr *member, const ClassDecl *owner,
                                     bool isViaThis) {
  auto set = this->classMemberRules.checkFieldSet(owner, member->name, isViaThis);
  if (set.isOk()) {
    return false;
  }
  this->state.diagnostics.error(member->loc(), set.error().message);

  return true;
}

MemberTargets::ResultFieldOfTheClass MemberTargets::checkFieldFound(const ClassField *found,
                                                                    const ClassDecl *classDecl,
                                                                    llvm::StringRef name) const {
  bool isMethod = found == nullptr && !classDecl->findStaticMethodsNamed(name).empty();
  if (isMethod) {
    auto message = ErrorTextStaticField::isMethod(name, classDecl->name);

    return err(CodeClassesRegistryFieldFoundError{
        .code = CodeClassesRegistryFieldFoundErrorCode::STATIC_METHOD,
        .message = message,
    });
  }
  if (found == nullptr) {
    auto message = ErrorTextStaticField::noField(classDecl->name, name);

    return err(CodeClassesRegistryFieldFoundError{
        .code = CodeClassesRegistryFieldFoundErrorCode::NO_STATIC_FIELD,
        .message = message,
    });
  }

  return ok(found);
}

MemberTargets::ResultFieldIsWritable
MemberTargets::checkStaticFieldWritable(const VarSymbol *variable) const {
  if (variable->isConst) {
    auto message = ErrorTextStaticField::readOnly(variable->name);

    return err(CodeClassesRegistryFieldWritableError{
        .code = CodeClassesRegistryFieldWritableErrorCode::READ_ONLY,
        .message = message,
    });
  }

  return ok(variable);
}

MemberTargets::ResultFieldTypeStated MemberTargets::checkTypeDeclared(const VarSymbol *variable,
                                                                      llvm::StringRef name) const {
  if (variable->type == nullptr) {
    auto message = ErrorTextStaticLater::typeOf(variable->name, name);

    return err(CodeClassesRegistryTypeDeclaredError{
        .code = CodeClassesRegistryTypeDeclaredErrorCode::TYPE_FROM_LATER_VALUE,
        .message = message,
    });
  }

  return ok(variable->type);
}

}
