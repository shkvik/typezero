#include "compiler/semantic/library/weakref/library_weakref_registry.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/weakref/weak_ref_method_table/weak_ref_method_table.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

LibraryWeakrefRegistry::LibraryWeakrefRegistry(SemanticState &owner)
    : state(owner) {}

void LibraryWeakrefRegistry::resolveWeakRefType(const ResolveWeakRefType &task) {
  const TypeRef &ref = *task.ref;
  auto &tasks = this->state.walk.tasks;
  auto written = this->checkTargetWritten(ref);
  if (written.isErr()) {
    this->state.diagnostics.error(ref.loc, written.error().message);

    return tasks.pushAnswer(Type::error());
  }
  tasks.pushTask(AfterWeakRefTarget{.ref = task.ref});

  return tasks.pushTask(ResolveTypeAnswer{.ref = written.value(), .use = TypeUse::STANDALONE});
}

void LibraryWeakrefRegistry::afterWeakRefTarget(const AfterWeakRefTarget &task) {
  auto &tasks = this->state.walk.tasks;
  const Type *target = tasks.takeAnswer();
  if (target->isError()) {
    return tasks.pushAnswer(target);
  }
  auto object = this->checkWeakTarget(target);
  if (object.isErr()) {
    this->state.diagnostics.error(task.ref->args[0].loc, object.error().message);

    return tasks.pushAnswer(Type::error());
  }

  return tasks.pushAnswer(Type::weakRef(target));
}

void LibraryWeakrefRegistry::checkWeakRefNew(const CheckWeakRefNew &task) {
  NewExpr *expr = task.newExpr;
  auto &tasks = this->state.walk.tasks;
  auto args = this->checkNewArgs(expr);
  if (args.isErr()) {
    this->state.diagnostics.error(expr->loc(), args.error().message);

    return this->failWeakRefNew(expr);
  }
  auto typeArgs = this->checkNewTypeArgs(expr);
  if (typeArgs.isErr()) {
    this->state.diagnostics.error(expr->loc(), typeArgs.error().message);

    return this->checkWeakRefValue(expr, nullptr);
  }
  if (expr->typeArgs.size() == 1) {
    tasks.pushTask(AfterWeakRefTypeArg{.newExpr = expr});

    return tasks.pushTask(ResolveTypeAnswer{.ref = &expr->typeArgs[0], .use = TypeUse::STANDALONE});
  }
  const Type *expected = task.expected;
  bool isExpected = expected != nullptr && expected->nonOptional()->isWeakRef();

  return this->checkWeakRefValue(expr,
                                 isExpected ? expected->nonOptional()->weakTarget() : nullptr);
}

void LibraryWeakrefRegistry::afterWeakRefTypeArg(const AfterWeakRefTypeArg &task) {
  const Type *written = this->state.walk.tasks.takeAnswer();

  return this->checkWeakRefValue(task.newExpr, written);
}

void LibraryWeakrefRegistry::checkWeakRefValue(NewExpr *expr, const Type *written) {
  auto &tasks = this->state.walk.tasks;
  if (written != nullptr && written->isError()) {
    return this->failWeakRefNew(expr);
  }
  Expr **slot = &expr->args[0];
  tasks.pushTask(AfterWeakRefValue{.newExpr = expr, .written = written});
  tasks.pushTask(ApparentSlot{.slot = slot});

  return tasks.pushTask(CheckExprSlot{.slot = slot, .expected = written});
}

void LibraryWeakrefRegistry::afterWeakRefValue(const AfterWeakRefValue &task) {
  NewExpr *expr = task.newExpr;
  auto &tasks = this->state.walk.tasks;
  Expr *arg = expr->args[0];
  if (arg->type->isError()) {
    return tasks.pushChecked(arg);
  }
  const Type *target = task.written != nullptr ? task.written : arg->type;
  auto object = this->checkWeakTarget(target);
  if (object.isErr()) {
    this->state.diagnostics.error(arg->loc(), object.error().message);

    return tasks.pushChecked(arg);
  }
  tasks.pushTask(FinishWeakRefNew{.newExpr = expr, .target = target});

  return tasks.pushTask(CoerceSlot{.slot = &expr->args[0], .to = target, .what = ""});
}

void LibraryWeakrefRegistry::finishWeakRefNew(const FinishWeakRefNew &task) {
  NewExpr *expr = task.newExpr;
  llvm::SMLoc loc = expr->loc();
  auto weakRefName = GlobalNameTable::getName(GlobalName::WEAK_REF);
  auto *callee = this->state.ast.make<NameExpr>(loc, weakRefName);
  const auto *weakRefType = Type::weakRef(task.target);
  auto *made = this->state.library.common.builtinCall(loc, callee, Builtin::WeakRefNew,
                                                      {expr->args[0]}, weakRefType);

  return this->state.walk.tasks.pushChecked(made);
}

void LibraryWeakrefRegistry::failWeakRefNew(NewExpr *expr) {
  auto &tasks = this->state.walk.tasks;
  auto *error = this->state.ast.make<UndefinedExpr>(expr->loc());
  error->type = Type::error();
  tasks.pushChecked(error);
  for (Expr *&arg : llvm::reverse(expr->args)) {
    tasks.pushTask(CheckExprSlot{.slot = &arg, .expected = nullptr});
  }
}

void LibraryWeakrefRegistry::checkWeakRefMethod(const CheckWeakRefMethod &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  const Type *type = member->object->type;
  call->type = Type::error();
  tasks.pushChecked(call);
  auto deref = this->checkMethodName(member, type);
  if (deref.isErr()) {
    this->state.diagnostics.error(member->loc(), deref.error().message);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  if (this->state.library.common.hasSpread(call)) {
    return tasks.pushTask(RejectSpreadCall{.call = call});
  }
  auto arity = this->state.library.common.checkArity(call, 0, 0);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  call->target = BuiltinCall{.builtin = Builtin::WeakRefDeref};
  call->type = Type::optional(type->weakTarget());
}

LibraryWeakrefRegistry::ResultTargetIsWritten
LibraryWeakrefRegistry::checkTargetWritten(const TypeRef &ref) const {
  if (ref.kind() != TypeRefKind::GENERIC_REF || ref.args.size() != 1) {
    auto message = ErrorTextWeakRef::noTarget();

    return err(LibraryWeakrefRegistryTargetWrittenError{
        .code = LibraryWeakrefRegistryTargetWrittenErrorCode::NO_TARGET_TYPE,
        .message = message,
    });
  }

  return ok(&ref.args[0]);
}

LibraryWeakrefRegistry::ResultWeakTargetValid
LibraryWeakrefRegistry::checkWeakTarget(const Type *target) const {
  bool isUnsupported = target->isRegExp() || target->isMatch() || target->isDate() ||
                       target->isDateFormat() || target->isBytes() || target->isTimer();
  if (isUnsupported) {
    auto message = ErrorTextWeakTarget::notYet(target->name());

    return err(LibraryWeakrefRegistryWeakTargetError{
        .code = LibraryWeakrefRegistryWeakTargetErrorCode::NOT_SUPPORTED_YET,
        .message = message,
    });
  }
  bool isObject = target->isObject() && !target->isAnyObject() && !target->isWeakRef();
  if (!isObject) {
    auto message = ErrorTextWeakTarget::notObject(target->name(), target->isOptional());

    return err(LibraryWeakrefRegistryWeakTargetError{
        .code = LibraryWeakrefRegistryWeakTargetErrorCode::NOT_AN_OBJECT,
        .message = message,
    });
  }

  return ok(target);
}

LibraryWeakrefRegistry::ResultNewTakesOneArgs
LibraryWeakrefRegistry::checkNewArgs(const NewExpr *expr) const {
  if (expr->args.size() != 1) {
    auto message = ErrorTextWeakRef::oneObject();

    return err(LibraryWeakrefRegistryNewArgsError{
        .code = LibraryWeakrefRegistryNewArgsErrorCode::NOT_ONE_OBJECT,
        .message = message,
    });
  }

  return ok(expr);
}

LibraryWeakrefRegistry::ResultNewTypeArgsFits
LibraryWeakrefRegistry::checkNewTypeArgs(const NewExpr *expr) const {
  if (expr->typeArgs.size() > 1) {
    auto message = ErrorTextWeakRef::typeArgs();

    return err(LibraryWeakrefRegistryNewTypeArgsError{
        .code = LibraryWeakrefRegistryNewTypeArgsErrorCode::SEVERAL_TYPE_ARGUMENTS,
        .message = message,
    });
  }

  return ok(expr);
}

LibraryWeakrefRegistry::ResultMethodIsDerefed
LibraryWeakrefRegistry::checkMethodName(const MemberExpr *member, const Type *type) const {
  auto method = WeakRefMethodTable::find(member->name);
  if (method != WeakRefMethod::DEREF) {
    auto message = ErrorTextWeakRef::noMethod(member->name, type->name());

    return err(LibraryWeakrefRegistryMethodNameError{
        .code = LibraryWeakrefRegistryMethodNameErrorCode::NO_SUCH_METHOD,
        .message = message,
    });
  }

  return ok(member);
}

}
