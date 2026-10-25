#include "compiler/library/weakref/weak_ref_checker/weak_ref_checker.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/library/process/process_checker/process_checker.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

WeakRefChecker::WeakRefChecker(Sema &sema) : sema_(sema) {}

const Type *WeakRefChecker::resolveWeakRefType(const TypeRef &ref) {
  if (ref.kind != TypeRef::Kind::Generic || ref.args.size() != 1) {
    sema_.diags_.error(ref.loc, "'WeakRef' needs the type of its object, e.g. 'WeakRef<Node>'");
    return Type::error();
  }
  const Type *target = sema_.typeResolver().resolveType(ref.args[0], /*allowVoid=*/false);
  if (target->isError()) {
    return target;
  }

  return checkWeakTarget(target, ref.args[0].loc) ? Type::weakRef(target) : Type::error();
}

// Only an object can be referred to weakly, as in JavaScript; a value of
// another type has no life of its own to end.
bool WeakRefChecker::checkWeakTarget(const Type *target, llvm::SMLoc loc) {
  // The objects the runtime makes and frees itself are not that yet.
  if (target->isRegExp() || target->isMatch() || target->isDate() || target->isDateFormat() ||
      target->isBytes() || target->isTimer()) {
    sema_.diags_.error(loc, "a WeakRef of '" + target->name().str() +
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
  sema_.diags_.error(loc, message);

  return false;
}

Expr *WeakRefChecker::checkWeakRefNew(NewExpr *expr, const Type *expected) {
  llvm::SMLoc loc = expr->loc();
  auto *callee = sema_.ctx_.make<NameExpr>(loc, "WeakRef");
  auto failed = [&] {
    for (Expr *&arg : expr->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }
    auto *error = sema_.ctx_.make<UndefinedExpr>(loc);
    error->type = Type::error();

    return error;
  };
  if (expr->args.size() != 1) {
    sema_.diags_.error(loc, "'new WeakRef' takes the object it refers to, e.g. 'new WeakRef(node)'");
    return failed();
  }
  // The object's type: as written, as the place wants it, or the
  // argument's.
  const Type *target = nullptr;
  if (expr->typeArgs.size() == 1) {
    target = sema_.typeResolver().resolveType(expr->typeArgs[0], /*allowVoid=*/false);
  } else if (!expr->typeArgs.empty()) {
    sema_.diags_.error(loc, "'WeakRef' takes one type argument, the type of its object");
  } else if (expected && expected->nonOptional()->isWeakRef()) {
    target = expected->nonOptional()->weakTarget();
  }
  if (target && target->isError()) {
    return failed();
  }
  Expr *arg = sema_.genericChecker().apparent(sema_.exprChecker().checkExpr(expr->args[0], target));
  if (arg->type->isError()) {
    return arg;
  }
  if (!target) {
    target = arg->type;
  }
  if (!checkWeakTarget(target, arg->loc())) {
    return arg;
  }
  arg = sema_.typeRelations().coerce(arg, target);

  return sema_.processChecker().builtinCall(loc, callee, Builtin::WeakRefNew, {arg}, Type::weakRef(target));
}

Expr *WeakRefChecker::checkWeakRefMethod(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  call->type = Type::error();
  if (member->name != "deref") {
    sema_.diags_.error(member->loc(), "property '" + member->name + "' does not exist on type '" +
                                    type->name() + "'; it has 'deref()'");
    for (Expr *&arg : call->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }
    return call;
  }
  if (sema_.callChecker().rejectSpread(call) || !sema_.argumentChecker().checkArity(call, member->loc(), 0, 0)) {
    return call;
  }
  call->builtin = Builtin::WeakRefDeref;
  call->type = Type::optional(type->weakTarget());

  return call;
}

} // namespace tz
