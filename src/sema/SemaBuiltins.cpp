// Built-in methods of strings and arrays, array literals and `new Array`.

#include "sema/Sema.h"

#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// The array variable a method is called on, if it is one.
const VarSymbol *arrayVariable(const Expr *object) {
  if (auto *conv = dyn_cast<CastExpr>(object);
      conv && conv->isImplicit() && conv->operand->type->isOptional())
    object = conv->operand;
  auto *name = dyn_cast<NameExpr>(object);
  if (!name || !name->var || !name->var->type->nonOptional()->isArray())
    return nullptr;
  return name->var;
}

std::string argumentCount(unsigned min, unsigned max) {
  std::string text = std::to_string(min);
  if (max != min)
    text += max == ~0u ? " or more" : "-" + std::to_string(max);
  return text + (max == 1 && min == 1 ? " argument" : " arguments");
}

} // namespace

Expr *Sema::checkStringMethod(CallExpr *call, MemberExpr *member) {
  struct Method {
    llvm::StringRef name;
    Builtin builtin;
    unsigned minArgs, maxArgs;
    bool firstArgIsString; // the other arguments are positions or counts
    const Type *ret;
  };
  static const Method methods[] = {
      {"charCodeAt", Builtin::StrCharCodeAt, 1, 1, false, Type::intTy()},
      {"at", Builtin::StrAt, 1, 1, false, Type::string()},
      {"slice", Builtin::StrSlice, 0, 2, false, Type::string()},
      {"substring", Builtin::StrSubstring, 1, 2, false, Type::string()},
      {"indexOf", Builtin::StrIndexOf, 1, 2, true, Type::intTy()},
      {"includes", Builtin::StrIncludes, 1, 2, true, Type::boolTy()},
      {"startsWith", Builtin::StrStartsWith, 1, 2, true, Type::boolTy()},
      {"endsWith", Builtin::StrEndsWith, 1, 2, true, Type::boolTy()},
      {"repeat", Builtin::StrRepeat, 1, 1, false, Type::string()},
      {"toString", Builtin::StrToString, 0, 0, false, Type::string()},
  };

  const Method *method = nullptr;
  for (const Method &m : methods)
    if (m.name == member->name)
      method = &m;
  if (!method) {
    diags_.error(member->loc(), "property '" + member->name + "' does not exist on type 'string'");
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    return call;
  }

  size_t count = call->args.size();
  if (count < method->minArgs || count > method->maxArgs) {
    diags_.error(member->loc(), "expected " + argumentCount(method->minArgs, method->maxArgs) +
                                    ", but got " + llvm::Twine(count));
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    return call;
  }

  for (size_t i = 0; i < count; ++i) {
    if (i == 0 && method->firstArgIsString)
      call->args[i] = coerce(checkExpr(call->args[i], Type::string()), Type::string());
    else
      call->args[i] = checkPosition(call->args[i]);
  }
  call->builtin = method->builtin;
  call->type = method->ret;

  if (method->builtin == Builtin::StrCharCodeAt) {
    // Out of range, JavaScript returns NaN, which only a double can hold.
    call->mayBeOutOfRange = !provesInBounds(member->object, call->args[0], /*allowEnd=*/false);
    call->type = call->mayBeOutOfRange ? Type::doubleTy() : Type::intTy();
  } else if (method->builtin == Builtin::StrAt) {
    call->mayBeOutOfRange = !provesAtInBounds(member->object, call->args[0]);
    call->type = call->mayBeOutOfRange ? Type::optional(Type::string()) : Type::string();
  }
  return call;
}

Expr *Sema::checkArrayMethod(CallExpr *call, MemberExpr *member) {
  const Type *arrayType = member->object->type;
  const Type *element = arrayType->inner();
  const VarSymbol *self = arrayVariable(member->object);
  llvm::StringRef name = member->name;
  size_t count = call->args.size();

  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
  };
  auto arity = [&](unsigned min, unsigned max) {
    if (count >= min && count <= max)
      return true;
    diags_.error(member->loc(), "expected " + argumentCount(min, max) + ", but got " +
                                    llvm::Twine(count));
    checkArgsLoosely();
    return false;
  };
  auto elementArg = [&](size_t i) {
    call->args[i] = coerce(checkExpr(call->args[i], element), element);
  };
  auto positionArgs = [&](size_t from) {
    for (size_t i = from; i < count; ++i)
      call->args[i] = checkPosition(call->args[i]);
  };

  static const char *const callbacks[] = {
      "map",  "filter", "forEach", "reduce",    "reduceRight", "find",    "findIndex",
      "some", "every",  "flatMap", "findLast",  "findLastIndex"};
  if (llvm::is_contained(callbacks, name)) {
    diags_.error(member->loc(), "'" + name +
                                    "' takes a callback, and arrow functions are not supported yet");
    checkArgsLoosely();
    return call;
  }

  if (name == "push" || name == "unshift") {
    for (size_t i = 0; i < count; ++i)
      elementArg(i);
    call->builtin = name == "push" ? Builtin::ArrPush : Builtin::ArrUnshift;
    call->type = Type::intTy();
    // The length grows by exactly `count`.
    if (self)
      flow_.facts.shift(Term::lengthOf(self), static_cast<int64_t>(count));
    arrayResized(arrayType, /*grew=*/true, /*shrank=*/false, self);
    return call;
  }

  if (name == "pop" || name == "shift") {
    if (!arity(0, 0))
      return call;
    call->builtin = name == "pop" ? Builtin::ArrPop : Builtin::ArrShift;
    bool notEmpty = provesNotEmpty(member->object);
    call->mayBeOutOfRange = !notEmpty;
    call->type = notEmpty ? element : Type::optional(element);
    if (self) {
      if (notEmpty)
        flow_.facts.shift(Term::lengthOf(self), -1);
      else
        flow_.facts.forgetIf(
            [&](const Facts::Fact &f) { return f.rhs == Term::lengthOf(self); });
    }
    arrayResized(arrayType, /*grew=*/false, /*shrank=*/true, self);
    return call;
  }

  if (name == "at") {
    if (!arity(1, 1))
      return call;
    positionArgs(0);
    call->builtin = Builtin::ArrAt;
    call->mayBeOutOfRange = !provesAtInBounds(member->object, call->args[0]);
    call->type = call->mayBeOutOfRange ? Type::optional(element) : element;
    return call;
  }

  if (name == "slice") {
    if (!arity(0, 2))
      return call;
    positionArgs(0);
    call->builtin = Builtin::ArrSlice;
    call->type = arrayType;
    return call;
  }

  if (name == "concat") {
    // Each argument is an array of the same type or a single element.
    for (size_t i = 0; i < count; ++i) {
      call->args[i] = checkExpr(call->args[i], arrayType);
      if (call->args[i]->type != arrayType)
        call->args[i] = coerce(call->args[i], element);
    }
    call->builtin = Builtin::ArrConcat;
    call->type = arrayType;
    return call;
  }

  if (name == "indexOf" || name == "lastIndexOf" || name == "includes") {
    if (!arity(1, 2))
      return call;
    elementArg(0);
    positionArgs(1);
    call->builtin = name == "indexOf"       ? Builtin::ArrIndexOf
                    : name == "lastIndexOf" ? Builtin::ArrLastIndexOf
                                            : Builtin::ArrIncludes;
    call->type = name == "includes" ? Type::boolTy() : Type::intTy();
    return call;
  }

  if (name == "join") {
    if (!arity(0, 1))
      return call;
    if (count == 1)
      call->args[0] = coerce(checkExpr(call->args[0], Type::string()), Type::string());
    call->builtin = Builtin::ArrJoin;
    call->type = Type::string();
    return call;
  }

  if (name == "reverse" || name == "toString") {
    if (!arity(0, 0))
      return call;
    call->builtin = name == "reverse" ? Builtin::ArrReverse : Builtin::ArrToString;
    call->type = name == "reverse" ? arrayType : Type::string();
    return call;
  }

  if (name == "fill") {
    if (!arity(1, 3))
      return call;
    elementArg(0);
    positionArgs(1);
    call->builtin = Builtin::ArrFill;
    call->type = arrayType;
    return call;
  }

  if (name == "sort") {
    if (count != 0) {
      diags_.error(member->loc(), "a comparison function needs arrow functions, which are not "
                                  "supported yet; 'sort()' without one compares as strings");
      checkArgsLoosely();
      return call;
    }
    call->builtin = Builtin::ArrSort;
    call->type = arrayType;
    return call;
  }

  if (name == "splice")
    diags_.error(member->loc(), "'splice' is not supported yet");
  else
    diags_.error(member->loc(), "property '" + name + "' does not exist on type '" +
                                    arrayType->name() + "'");
  checkArgsLoosely();
  return call;
}

Expr *Sema::checkNew(NewExpr *expr, const Type *expected) {
  expr->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : expr->args)
      arg = checkExpr(arg);
  };
  if (expr->name != "Array") {
    diags_.error(expr->loc(), "'new " + expr->name + "' is not supported; only 'new Array<T>(length)' is");
    checkArgsLoosely();
    return expr;
  }

  const Type *element = nullptr;
  if (expr->typeArgs.size() == 1) {
    element = resolveType(expr->typeArgs[0], /*allowVoid=*/false);
  } else if (expr->typeArgs.size() > 1) {
    diags_.error(expr->loc(), "'Array' takes one type argument");
    element = Type::error();
  } else if (expected && expected->nonOptional()->isArray()) {
    element = expected->nonOptional()->inner();
  } else {
    diags_.error(expr->loc(), "'new Array' needs an element type, e.g. 'new Array<int>(n)'");
    element = Type::error();
  }

  if (expr->args.size() > 1) {
    diags_.error(expr->loc(), "to create an array from values, use a literal: [a, b, c]");
    checkArgsLoosely();
    return expr;
  }
  if (expr->args.size() == 1) {
    expr->args[0] = checkPosition(expr->args[0]);
    if (!element->isError() && !element->hasZeroValue())
      diags_.error(expr->loc(), "'new Array<T>(n)' fills the array with a default value, and '" +
                                    element->name() + "' has none; start from '[]' and use push()");
    if (std::optional<Linear> length = linear(expr->args[0]);
        length && length->term.isZero() && length->offset < 0)
      diags_.error(expr->args[0]->loc(), "an array length cannot be negative");
  }
  if (!element->isError())
    expr->type = Type::array(element);
  return expr;
}

Expr *Sema::checkArrayLit(ArrayLitExpr *lit, const Type *expected) {
  const Type *expectedElement = nullptr;
  if (expected && expected->nonOptional()->isArray())
    expectedElement = expected->nonOptional()->inner();

  // The element types: of the values, or of the arrays spread into this one.
  std::vector<const Type *> types;
  for (ArrayLitExpr::Element &e : lit->elements) {
    if (!e.spread) {
      e.expr = checkExpr(e.expr, expectedElement);
      types.push_back(e.expr->type);
      continue;
    }
    e.expr = checkExpr(e.expr, expectedElement ? Type::array(expectedElement) : nullptr);
    const Type *type = e.expr->type;
    if (!type->isArray() && !type->isError()) {
      diags_.error(e.expr->loc(), "only arrays can be spread here, found '" + type->name() + "'");
      type = Type::error();
    }
    types.push_back(type->isError() ? type : type->inner());
  }

  lit->type = Type::error();
  const Type *element = expectedElement;
  if (!element) {
    if (lit->elements.empty()) {
      diags_.error(lit->loc(), "cannot infer the element type of an empty array; add a type, "
                               "e.g. 'let a: int[] = []'");
      return lit;
    }
    for (size_t i = 0; i < types.size(); ++i) {
      if (types[i]->isError())
        return lit;
      const Type *next = element ? unify(element, types[i]) : types[i];
      if (!next) {
        diags_.error(lit->elements[i].expr->loc(), "array elements must have one type; found '" +
                                                       element->name() + "' and '" +
                                                       types[i]->name() + "'");
        return lit;
      }
      element = next;
    }
    if (element->isUndefined() || element->isVoid()) {
      diags_.error(lit->loc(), "cannot infer the element type from '" + element->name() +
                                   "' alone; add a type, e.g. 'let a: (int | undefined)[] = ...'");
      return lit;
    }
  }
  if (element->isError())
    return lit;

  const Type *arrayType = Type::array(element);
  for (ArrayLitExpr::Element &e : lit->elements) {
    if (!e.spread) {
      e.expr = coerce(e.expr, element);
    } else if (e.expr->type != arrayType && !e.expr->type->isError()) {
      // Arrays of different types have different layouts, so no conversion.
      diags_.error(e.expr->loc(), "cannot spread '" + e.expr->type->name() + "' into '" +
                                      arrayType->name() + "'");
    }
  }
  lit->type = arrayType;
  return lit;
}

} // namespace tz
