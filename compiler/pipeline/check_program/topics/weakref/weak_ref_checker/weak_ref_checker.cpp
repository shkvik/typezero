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

const Type *WeakRefChecker::resolveWeakRefType(const TypeRef &ref) {
  if (ref.kind() != TypeRefKind::GENERIC_REF || ref.args.size() != 1) {
    this->state.getDiagnostics().error(
        ref.loc, "'WeakRef' needs the type of its object, e.g. 'WeakRef<Node>'");
    return Type::error();
  }
  const Type *target = TypeResolver(this->state).resolveType(ref.args[0], false);
  if (target->isError()) {
    return target;
  }

  return this->checkWeakTarget(target, ref.args[0].loc) ? Type::weakRef(target) : Type::error();
}

bool WeakRefChecker::checkWeakTarget(const Type *target, llvm::SMLoc loc) {
  if (target->isRegExp() || target->isMatch() || target->isDate() || target->isDateFormat() ||
      target->isBytes() || target->isTimer()) {
    this->state.getDiagnostics().error(
        loc, "a WeakRef of '" + target->name().str() +
                 "' is not supported yet; it refers to objects of the program's types");
    return false;
  }
  if (target->isObject() && !target->isAnyObject() && !target->isWeakRef()) {
    return true;
  }
  std::string message =
      "a WeakRef refers to an object, not to a value of type '" + target->name().str() + "'";
  if (target->isOptional()) {
    message += "; make it of the object and check for undefined before";
  } else {
    message += "; hold the value itself";
  }
  this->state.getDiagnostics().error(loc, message);

  return false;
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
  if (expr->args.size() != 1) {
    this->state.getDiagnostics().error(
        loc, "'new WeakRef' takes the object it refers to, e.g. 'new WeakRef(node)'");
    return failed();
  }
  const Type *target = nullptr;
  if (expr->typeArgs.size() == 1) {
    target = TypeResolver(this->state).resolveType(expr->typeArgs[0], false);
  } else if (!expr->typeArgs.empty()) {
    this->state.getDiagnostics().error(loc,
                                       "'WeakRef' takes one type argument, the type of its object");
  } else if (expected && expected->nonOptional()->isWeakRef()) {
    target = expected->nonOptional()->weakTarget();
  }
  if (target && target->isError()) {
    return failed();
  }
  Expr *arg = GenericChecker(this->state)
                  .apparent(ExprChecker(this->state).checkExpr(expr->args[0], target));
  if (arg->type->isError()) {
    return arg;
  }
  if (!target) {
    target = arg->type;
  }
  if (!this->checkWeakTarget(target, arg->loc())) {
    return arg;
  }
  arg = TypeRelations(this->state).coerce(arg, target);

  return ProcessChecker(this->state)
      .builtinCall(loc, callee, Builtin::WeakRefNew, {arg}, Type::weakRef(target));
}

Expr *WeakRefChecker::checkWeakRefMethod(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  call->type = Type::error();
  if (member->name != "deref") {
    this->state.getDiagnostics().error(member->loc(), "property '" + member->name +
                                                          "' does not exist on type '" +
                                                          type->name() + "'; it has 'deref()'");
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
