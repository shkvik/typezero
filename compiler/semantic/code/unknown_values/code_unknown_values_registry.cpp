#include "compiler/semantic/code/unknown_values/code_unknown_values_registry.h"
#include "compiler/program/library/builtin_names/type_of_name_table/type_of_name_table.h"
#include "compiler/program/library/object/object_member_table/object_member_table.h"
#include "compiler/program/types/type_spelling/type_spelling.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

CodeUnknownValuesRegistry::CodeUnknownValuesRegistry(SemanticState &owner)
    : state(owner) {}

void CodeUnknownValuesRegistry::checkUnknownMember(const CheckUnknownMember &task) {
  MemberExpr *member = task.member;
  member->type = Type::error();
  if (member->object->type->isClassRef()) {
    return this->state.walk.tasks.pushTask(CheckClassRefMember{.member = member});
  }
  this->state.walk.tasks.pushChecked(member);
  auto constructor = this->checkNotConstructor(member);
  if (constructor.isErr()) {
    return this->state.diagnostics.error(member->loc(), constructor.error().message);
  }
  auto what = ErrorTextUnknownUse::property(member->name);
  auto narrowed = this->unknownUse(member->object, what);

  return this->state.diagnostics.error(member->object->loc(), narrowed.error().message);
}

void CodeUnknownValuesRegistry::checkUnknownMethod(const CheckUnknownMethod &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  const Type *receiver = member->object->type;
  call->type = Type::error();
  auto *classDecl = const_cast<ClassDecl *>(receiver->classRef());
  if (classDecl != nullptr && classDecl->type != nullptr) {
    this->state.walk.tasks.pushTask(FinishUnknownStaticCall{.call = call, .member = member});
    return this->state.walk.tasks.pushTask(CheckClassStaticCall{
        .call = call,
        .member = member,
        .classDecl = classDecl,
        .expected = nullptr,
    });
  }
  this->state.walk.tasks.pushTask(FinishUnknownMethod{.call = call, .member = member});

  return this->state.walk.tasks.pushTask(WalkArgsLoosely{.call = call});
}

void CodeUnknownValuesRegistry::finishUnknownStaticCall(const FinishUnknownStaticCall &task) {
  Expr *result = this->state.walk.tasks.takeChecked();
  Expr *object = task.member->object;
  if (isa<NameExpr>(object)) {
    return this->state.walk.tasks.pushChecked(result);
  }
  auto parts = std::vector<Expr *>{object, result};
  auto *sequence = this->state.ast.make<SequenceExpr>(task.call->loc(), parts);
  sequence->type = result->type;

  return this->state.walk.tasks.pushChecked(sequence);
}

void CodeUnknownValuesRegistry::finishUnknownMethod(const FinishUnknownMethod &task) {
  MemberExpr *member = task.member;
  const Type *receiver = member->object->type;
  this->state.walk.tasks.pushChecked(task.call);
  auto runtimeClass = this->checkNotRuntimeClass(receiver);
  if (runtimeClass.isErr()) {
    return this->state.diagnostics.error(member->loc(), runtimeClass.error().message);
  }
  auto classValue = this->checkNotClassValue(receiver);
  if (classValue.isErr()) {
    return this->state.diagnostics.error(member->loc(), classValue.error().message);
  }
  auto what = ErrorTextUnknownUse::method(member->name);
  auto narrowed = this->unknownUse(member->object, what);

  return this->state.diagnostics.error(member->object->loc(), narrowed.error().message);
}

std::string CodeUnknownValuesRegistry::hintName(const Expr *expr) const {
  const Expr *bare = ExprParts::withoutImplicitCasts(expr);
  auto *name = dyn_cast<NameExpr>(bare);
  if (name == nullptr) {
    return "x";
  }

  return name->name.str();
}

CodeUnknownValuesRegistry::ResultValueNarrowed
CodeUnknownValuesRegistry::unknownUse(const Expr *object, llvm::StringRef what) const {
  auto shown = this->hintName(object);
  if (object->type->isAnyObject()) {
    auto message = ErrorTextUnknownUse::object(shown, what);

    return err(CodeUnknownValuesRegistryUnknownUseError{
        .code = CodeUnknownValuesRegistryUnknownUseErrorCode::OBJECT_NOT_NARROWED,
        .message = message,
    });
  }
  auto message = ErrorTextUnknownUse::unknown(shown, what);

  return err(CodeUnknownValuesRegistryUnknownUseError{
      .code = CodeUnknownValuesRegistryUnknownUseErrorCode::VALUE_NOT_NARROWED,
      .message = message,
  });
}

CodeUnknownValuesRegistry::ResultMemberAllowed
CodeUnknownValuesRegistry::checkNotConstructor(const MemberExpr *member) const {
  if (member->name == ObjectMemberTable::getName(ObjectMember::CONSTRUCTOR)) {
    auto message = ErrorTextUnknownMember::noCtor();

    return err(CodeUnknownValuesRegistryCheckUnknownMemberError{
        .code = CodeUnknownValuesRegistryCheckUnknownMemberErrorCode::NO_CONSTRUCTOR,
        .message = message,
    });
  }

  return ok();
}

CodeUnknownValuesRegistry::ResultMethodAllowed
CodeUnknownValuesRegistry::checkNotRuntimeClass(const Type *receiver) const {
  if (receiver->isConstructor()) {
    auto message = ErrorTextUnknownMethod::runtime(receiver->name());

    return err(CodeUnknownValuesRegistryCheckUnknownMethodError{
        .code = CodeUnknownValuesRegistryCheckUnknownMethodErrorCode::RUNTIME_CLASS,
        .message = message,
    });
  }

  return ok();
}

CodeUnknownValuesRegistry::ResultMethodAllowed
CodeUnknownValuesRegistry::checkNotClassValue(const Type *receiver) const {
  if (receiver->isClassRef()) {
    auto message = ErrorTextUnknownMethod::classValue(receiver->classRef()->name);

    return err(CodeUnknownValuesRegistryCheckUnknownMethodError{
        .code = CodeUnknownValuesRegistryCheckUnknownMethodErrorCode::CLASS_VALUE,
        .message = message,
    });
  }

  return ok();
}

}
