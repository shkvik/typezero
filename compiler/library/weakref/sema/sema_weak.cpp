// WeakRef (docs/objects.md, section 11): a reference to an object that does
// not hold it. `deref()` gives the object, or undefined from the moment it
// died, which in tz is exactly when its count or that of its group reached
// zero (runtime/group.c).

#include "sema/sema.h"

namespace tz {

const Type *Sema::resolveWeakRefType(const TypeRef &ref) {
  if (ref.kind != TypeRef::Kind::Generic || ref.args.size() != 1) {
    diags_.error(ref.loc, "'WeakRef' needs the type of its object, e.g. 'WeakRef<Node>'");
    return Type::error();
  }
  const Type *target = resolveType(ref.args[0], /*allowVoid=*/false);
  if (target->isError())
    return target;
  return checkWeakTarget(target, ref.args[0].loc) ? Type::weakRef(target) : Type::error();
}

// Only an object can be referred to weakly, as in JavaScript; a value of
// another type has no life of its own to end.
bool Sema::checkWeakTarget(const Type *target, llvm::SMLoc loc) {
  // The objects the runtime makes and frees itself are not that yet.
  if (target->isRegExp() || target->isMatch() || target->isDate() || target->isDateFormat() ||
      target->isBytes() || target->isTimer()) {
    diags_.error(loc, "a WeakRef of '" + target->name().str() +
                          "' is not supported yet; it refers to objects of the program's types");
    return false;
  }
  if (target->isObject() && !target->isAnyObject() && !target->isWeakRef())
    return true;
  std::string message = "a WeakRef refers to an object, not to a value of type '" +
                        target->name().str() + "'";
  if (target->isOptional())
    message += "; make it of the object and check for undefined before";
  else
    message += "; hold the value itself";
  diags_.error(loc, message);
  return false;
}

Expr *Sema::checkWeakRefNew(NewExpr *expr, const Type *expected) {
  llvm::SMLoc loc = expr->loc();
  auto *callee = ctx_.make<NameExpr>(loc, "WeakRef");
  auto failed = [&] {
    for (Expr *&arg : expr->args)
      arg = checkExpr(arg);
    auto *error = ctx_.make<UndefinedExpr>(loc);
    error->type = Type::error();
    return error;
  };
  if (expr->args.size() != 1) {
    diags_.error(loc, "'new WeakRef' takes the object it refers to, e.g. 'new WeakRef(node)'");
    return failed();
  }
  // The object's type: as written, as the place wants it, or the
  // argument's.
  const Type *target = nullptr;
  if (expr->typeArgs.size() == 1)
    target = resolveType(expr->typeArgs[0], /*allowVoid=*/false);
  else if (!expr->typeArgs.empty())
    diags_.error(loc, "'WeakRef' takes one type argument, the type of its object");
  else if (expected && expected->nonOptional()->isWeakRef())
    target = expected->nonOptional()->weakTarget();
  if (target && target->isError())
    return failed();
  Expr *arg = apparent(checkExpr(expr->args[0], target));
  if (arg->type->isError())
    return arg;
  if (!target)
    target = arg->type;
  if (!checkWeakTarget(target, arg->loc()))
    return arg;
  arg = coerce(arg, target);
  return builtinCall(loc, callee, Builtin::WeakRefNew, {arg}, Type::weakRef(target));
}

Expr *Sema::checkWeakRefMethod(CallExpr *call, MemberExpr *member) {
  const Type *type = member->object->type;
  call->type = Type::error();
  if (member->name != "deref") {
    diags_.error(member->loc(), "property '" + member->name + "' does not exist on type '" +
                                    type->name() + "'; it has 'deref()'");
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    return call;
  }
  if (rejectSpread(call) || !checkArity(call, member->loc(), 0, 0))
    return call;
  call->builtin = Builtin::WeakRefDeref;
  call->type = Type::optional(type->weakTarget());
  return call;
}

} // namespace tz
