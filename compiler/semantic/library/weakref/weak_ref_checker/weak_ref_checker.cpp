#include "compiler/semantic/library/weakref/weak_ref_checker/weak_ref_checker.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/library/process/process_checker/process_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"

namespace tz {

WeakRefChecker::WeakRefChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->typeResolver = &all.typeResolver;
  this->typeRelations = &all.typeRelations;
  this->genericChecker = &all.genericChecker;
  this->processChecker = &all.processChecker;
  this->argumentChecker = &all.argumentChecker;
}

WeakRefChecker::ResultTargetIsWritten WeakRefChecker::targetWritten(const TypeRef &ref) const {
  if (ref.kind() != TypeRefKind::GENERIC_REF || ref.args.size() != 1) {
    auto message = ErrorTextWeakRef::noTarget();

    return err(WeakRefCheckerTargetWrittenError{
        .code = WeakRefCheckerTargetWrittenErrorCode::NO_TARGET_TYPE,
        .message = message,
    });
  }

  return ok(&ref.args[0]);
}

WeakRefChecker::ResultWeakTargetValid WeakRefChecker::weakTarget(const Type *target) const {
  bool isUnsupported = target->isRegExp() || target->isMatch() || target->isDate() ||
                       target->isDateFormat() || target->isBytes() || target->isTimer();
  if (isUnsupported) {
    auto message = ErrorTextWeakTarget::notYet(target->name());

    return err(WeakRefCheckerWeakTargetError{
        .code = WeakRefCheckerWeakTargetErrorCode::NOT_SUPPORTED_YET,
        .message = message,
    });
  }
  bool isObject = target->isObject() && !target->isAnyObject() && !target->isWeakRef();
  if (!isObject) {
    auto message = ErrorTextWeakTarget::notObject(target->name(), target->isOptional());

    return err(WeakRefCheckerWeakTargetError{
        .code = WeakRefCheckerWeakTargetErrorCode::NOT_AN_OBJECT,
        .message = message,
    });
  }

  return ok(target);
}

WeakRefChecker::ResultNewTakesOneArgs WeakRefChecker::newArgs(const NewExpr *expr) const {
  if (expr->args.size() != 1) {
    auto message = ErrorTextWeakRef::oneObject();

    return err(WeakRefCheckerNewArgsError{
        .code = WeakRefCheckerNewArgsErrorCode::NOT_ONE_OBJECT,
        .message = message,
    });
  }

  return ok(expr);
}

WeakRefChecker::ResultNewTypeArgsFits WeakRefChecker::newTypeArgs(const NewExpr *expr) const {
  if (expr->typeArgs.size() > 1) {
    auto message = ErrorTextWeakRef::typeArgs();

    return err(WeakRefCheckerNewTypeArgsError{
        .code = WeakRefCheckerNewTypeArgsErrorCode::SEVERAL_TYPE_ARGUMENTS,
        .message = message,
    });
  }

  return ok(expr);
}

WeakRefChecker::ResultMethodIsDerefed WeakRefChecker::methodName(const MemberExpr *member,
                                                                 const Type *type) const {
  if (member->name != "deref") {
    auto message = ErrorTextWeakRef::noMethod(member->name, type->name());

    return err(WeakRefCheckerMethodNameError{
        .code = WeakRefCheckerMethodNameErrorCode::NO_SUCH_METHOD,
        .message = message,
    });
  }

  return ok(member);
}

const Type *WeakRefChecker::resolveWeakRefType(const TypeRef &ref) {
  auto written = this->targetWritten(ref);
  if (written.isErr()) {
    this->state.getDiagnostics().error(ref.loc, written.error().message);
    return Type::error();
  }
  const TypeRef &targetRef = *written.value();
  const auto *target = this->typeResolver->resolveType(targetRef);
  if (target->isError()) {
    return target;
  }
  auto object = this->weakTarget(target);
  if (object.isErr()) {
    this->state.getDiagnostics().error(targetRef.loc, object.error().message);
    return Type::error();
  }

  return Type::weakRef(target);
}

Expr *WeakRefChecker::checkWeakRefNew(NewExpr *expr, const Type *expected) {
  llvm::SMLoc loc = expr->loc();
  auto *callee = this->state.getAst().make<NameExpr>(loc, "WeakRef");
  auto failed = [&] {
    for (Expr *&arg : expr->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    auto *error = this->state.getAst().make<UndefinedExpr>(loc);
    error->type = Type::error();

    return error;
  };
  auto args = this->newArgs(expr);
  if (args.isErr()) {
    this->state.getDiagnostics().error(loc, args.error().message);
    return failed();
  }
  const Type *target = nullptr;
  auto typeArgs = this->newTypeArgs(expr);
  if (typeArgs.isErr()) {
    this->state.getDiagnostics().error(loc, typeArgs.error().message);
  } else if (expr->typeArgs.size() == 1) {
    target = this->typeResolver->resolveType(expr->typeArgs[0]);
  } else if (expected != nullptr && expected->nonOptional()->isWeakRef()) {
    target = expected->nonOptional()->weakTarget();
  }
  if (target != nullptr && target->isError()) {
    return failed();
  }
  auto *checked = this->exprChecker->checkExpr(expr->args[0], target);
  auto *arg = this->genericChecker->apparent(checked);
  if (arg->type->isError()) {
    return arg;
  }
  if (target == nullptr) {
    target = arg->type;
  }
  auto object = this->weakTarget(target);
  if (object.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), object.error().message);
    return arg;
  }
  arg = this->typeRelations->coerce(arg, target);

  return this->processChecker->builtinCall(loc, callee, Builtin::WeakRefNew, {arg},
                                           Type::weakRef(target));
}

Expr *WeakRefChecker::checkWeakRefMethod(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  call->type = Type::error();
  auto deref = this->methodName(member, type);
  if (deref.isErr()) {
    this->state.getDiagnostics().error(member->loc(), deref.error().message);
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return call;
  }
  bool isRejected = this->callChecker->rejectSpread(call);
  auto arity = isRejected ? ok(call) : this->argumentChecker->checkArity(call, 0, 0);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
  }
  if (isRejected || arity.isErr()) {
    return call;
  }
  call->target = BuiltinCall{.builtin = Builtin::WeakRefDeref};
  call->type = Type::optional(type->weakTarget());

  return call;
}

}
