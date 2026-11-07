#include "compiler/pipeline/check_program/topics/weakref/weak_ref_checker/weak_ref_checker.h"
#include "compiler/pipeline/check_program/generics/generic_checker/generic_checker.h"
#include "compiler/pipeline/check_program/topics/expr/argument_checker/argument_checker.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/process/process_checker/process_checker.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "compiler/pipeline/check_program/types/type_resolver/type_resolver.h"

namespace tz {

WeakRefChecker::WeakRefChecker(CheckState &owner)
    : state(owner) {}

WeakRefChecker::ResultTargetIsWritten WeakRefChecker::targetWritten(const TypeRef &ref) {
  if (ref.kind() != TypeRefKind::GENERIC_REF || ref.args.size() != 1) {
    std::string message = ErrorTextWeakRef::noTarget();

    return err(WeakRefCheckerTargetWrittenError{
        .code = WeakRefCheckerTargetWrittenErrorCode::NO_TARGET_TYPE,
        .message = message,
    });
  }

  return ok(&ref.args[0]);
}

WeakRefChecker::ResultWeakTargetValid WeakRefChecker::weakTarget(const Type *target) {
  bool unsupported = target->isRegExp() || target->isMatch() || target->isDate() ||
                     target->isDateFormat() || target->isBytes() || target->isTimer();
  if (unsupported) {
    std::string message = ErrorTextWeakTarget::notYet(target->name());

    return err(WeakRefCheckerWeakTargetError{
        .code = WeakRefCheckerWeakTargetErrorCode::NOT_SUPPORTED_YET,
        .message = message,
    });
  }
  bool object = target->isObject() && !target->isAnyObject() && !target->isWeakRef();
  if (!object) {
    std::string message = ErrorTextWeakTarget::notObject(target->name(), target->isOptional());

    return err(WeakRefCheckerWeakTargetError{
        .code = WeakRefCheckerWeakTargetErrorCode::NOT_AN_OBJECT,
        .message = message,
    });
  }

  return ok(target);
}

WeakRefChecker::ResultNewTakesOneArgs WeakRefChecker::newArgs(const NewExpr *expr) {
  if (expr->args.size() != 1) {
    std::string message = ErrorTextWeakRef::oneObject();

    return err(WeakRefCheckerNewArgsError{
        .code = WeakRefCheckerNewArgsErrorCode::NOT_ONE_OBJECT,
        .message = message,
    });
  }

  return ok(expr);
}

WeakRefChecker::ResultNewTypeArgsFits WeakRefChecker::newTypeArgs(const NewExpr *expr) {
  if (expr->typeArgs.size() > 1) {
    std::string message = ErrorTextWeakRef::typeArgs();

    return err(WeakRefCheckerNewTypeArgsError{
        .code = WeakRefCheckerNewTypeArgsErrorCode::SEVERAL_TYPE_ARGUMENTS,
        .message = message,
    });
  }

  return ok(expr);
}

WeakRefChecker::ResultMethodIsDerefed WeakRefChecker::methodName(const MemberExpr *member,
                                                                 const Type *type) {
  if (member->name != "deref") {
    std::string message = ErrorTextWeakRef::noMethod(member->name, type->name());

    return err(WeakRefCheckerMethodNameError{
        .code = WeakRefCheckerMethodNameErrorCode::NO_SUCH_METHOD,
        .message = message,
    });
  }

  return ok(member);
}

const Type *WeakRefChecker::resolveWeakRefType(const TypeRef &ref) {
  ResultTargetIsWritten written = WeakRefChecker::targetWritten(ref);
  if (written.isErr()) {
    this->state.getDiagnostics().error(ref.loc, written.error().message);
    return Type::error();
  }
  const TypeRef &targetRef = *written.value();
  const Type *target = TypeResolver(this->state).resolveType(targetRef, false);
  if (target->isError()) {
    return target;
  }
  ResultWeakTargetValid object = WeakRefChecker::weakTarget(target);
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
      arg = ExprChecker(this->state).checkExpr(arg);
    }
    auto *error = this->state.getAst().make<UndefinedExpr>(loc);
    error->type = Type::error();

    return error;
  };
  ResultNewTakesOneArgs args = WeakRefChecker::newArgs(expr);
  if (args.isErr()) {
    this->state.getDiagnostics().error(loc, args.error().message);
    return failed();
  }
  const Type *target = nullptr;
  ResultNewTypeArgsFits typeArgs = WeakRefChecker::newTypeArgs(expr);
  if (typeArgs.isErr()) {
    this->state.getDiagnostics().error(loc, typeArgs.error().message);
  } else if (expr->typeArgs.size() == 1) {
    target = TypeResolver(this->state).resolveType(expr->typeArgs[0], false);
  } else if (expected && expected->nonOptional()->isWeakRef()) {
    target = expected->nonOptional()->weakTarget();
  }
  if (target && target->isError()) {
    return failed();
  }
  Expr *checked = ExprChecker(this->state).checkExpr(expr->args[0], target);
  Expr *arg = GenericChecker(this->state).apparent(checked);
  if (arg->type->isError()) {
    return arg;
  }
  if (!target) {
    target = arg->type;
  }
  ResultWeakTargetValid object = WeakRefChecker::weakTarget(target);
  if (object.isErr()) {
    this->state.getDiagnostics().error(arg->loc(), object.error().message);
    return arg;
  }
  arg = TypeRelations(this->state).coerce(arg, target);

  return ProcessChecker(this->state)
      .builtinCall(loc, callee, Builtin::WeakRefNew, {arg}, Type::weakRef(target));
}

Expr *WeakRefChecker::checkWeakRefMethod(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  call->type = Type::error();
  ResultMethodIsDerefed deref = WeakRefChecker::methodName(member, type);
  if (deref.isErr()) {
    this->state.getDiagnostics().error(member->loc(), deref.error().message);
    for (Expr *&arg : call->args) {
      arg = ExprChecker(this->state).checkExpr(arg);
    }
    return call;
  }
  bool rejected = CallChecker(this->state).rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : ArgumentChecker(this->state).checkArity(call, 0, 0);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
  }
  if (rejected || arity.isErr()) {
    return call;
  }
  call->target = BuiltinCall{.builtin = Builtin::WeakRefDeref};
  call->type = Type::optional(type->weakTarget());

  return call;
}

}
