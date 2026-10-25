// Built-in functions: methods of strings, arrays and numbers, array literals,
// `new Array`, the functions of Math, Number and String, and parseInt and
// the like.

#include "sema/sema.h"

#include "llvm/ADT/STLExtras.h"

#include <cstring>

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

// The type a call that gives a whole number has where `expected` is
// expected: an int or a long when one is expected, which the program checks
// the number fits in (JavaScript would go on with a number that type cannot
// hold), and otherwise `type` itself.
const Type *wholeNumberType(const Type *type, const Type *expected) {
  const Type *wanted = expected ? expected->nonOptional() : nullptr;
  bool narrows = wanted && wanted->isInteger() &&
                 (type->isFloat() || type->bitWidth() > wanted->bitWidth());
  return narrows ? wanted : type;
}

} // namespace

std::string argumentCount(unsigned min, unsigned max) {
  std::string text = std::to_string(min);
  if (max != min)
    text += max == ~0u ? " or more" : "-" + std::to_string(max);
  return text + (max == 1 && min == 1 ? " argument" : " arguments");
}

const char *whyUnsupported(llvm::StringRef name) {
  if (name == "eval")
    return "a compiled program cannot run code from a string";
  if (name == "Function")
    return "a compiled program cannot run code from a string; write the function itself, e.g. "
           "'(a: int) => a'";
  if (name == "Proxy")
    return "the fields of an object are read and written straight in memory, so nothing can "
           "intercept that";
  if (name == "require")
    return "files are imported by name: 'import { a } from \"./x\"'";
  if (name == "module" || name == "exports")
    return "files export by name: 'export function f() {...}', or 'export { a, b }'";
  return nullptr;
}

Expr *Sema::numericArg(Expr *arg, const Type *hint) {
  arg = apparent(checkExpr(arg, hint));
  const Type *type = arg->type;
  if (!type->isNumeric() && !type->isError()) {
    std::string message = "argument of type '" + type->name().str() +
                          "' is not assignable to parameter of type 'double'";
    if (type->isOptional() && type->inner()->isNumeric())
      message += "; the value may be undefined: check it first or give a default with '??'";
    else if (type->isDecimal())
      message += "; convert it with 'Number(x)', or use what decimals have: 'x.abs()', 'x.sqrt()', "
                 "'x.round()', 'decimal.max(a, b)'...";
    diags_.error(arg->loc(), message);
  }
  return arg;
}

// The functions of Math, which work as in JavaScript. Most take and return
// doubles. Others keep the type of their arguments: abs, sign, min and max
// of ints give an int, and pow is `**`. floor, ceil, round and trunc give a
// double, or an int or a long where one is expected (see wholeNumberType).
Expr *Sema::checkMathCall(CallExpr *call, MemberExpr *member, const Type *expected) {
  enum Kind {
    Rounding, // floor, ceil, round, trunc
    Float,    // doubles in, a double out
    Same,     // the type of the argument out: abs, sign
    MinMax,   // the common type of the arguments
    Pow,
    Int32,    // ToInt32 of the arguments, an int out: clz32, imul
  };
  struct Function {
    llvm::StringLiteral name;
    Builtin builtin;
    Kind kind;
    unsigned minArgs, maxArgs;
  };
  constexpr unsigned kAny = ~0u;
  static const Function functions[] = {
      {"floor", Builtin::MathFloor, Rounding, 1, 1},  {"ceil", Builtin::MathCeil, Rounding, 1, 1},
      {"round", Builtin::MathRound, Rounding, 1, 1},  {"trunc", Builtin::MathTrunc, Rounding, 1, 1},
      {"abs", Builtin::MathAbs, Same, 1, 1},          {"sign", Builtin::MathSign, Same, 1, 1},
      {"min", Builtin::MathMin, MinMax, 0, kAny},     {"max", Builtin::MathMax, MinMax, 0, kAny},
      {"pow", Builtin::MathPow, Pow, 2, 2},           {"sqrt", Builtin::MathSqrt, Float, 1, 1},
      {"cbrt", Builtin::MathCbrt, Float, 1, 1},       {"exp", Builtin::MathExp, Float, 1, 1},
      {"expm1", Builtin::MathExpm1, Float, 1, 1},     {"log", Builtin::MathLog, Float, 1, 1},
      {"log1p", Builtin::MathLog1p, Float, 1, 1},     {"log2", Builtin::MathLog2, Float, 1, 1},
      {"log10", Builtin::MathLog10, Float, 1, 1},     {"sin", Builtin::MathSin, Float, 1, 1},
      {"cos", Builtin::MathCos, Float, 1, 1},         {"tan", Builtin::MathTan, Float, 1, 1},
      {"asin", Builtin::MathAsin, Float, 1, 1},       {"acos", Builtin::MathAcos, Float, 1, 1},
      {"atan", Builtin::MathAtan, Float, 1, 1},       {"atan2", Builtin::MathAtan2, Float, 2, 2},
      {"sinh", Builtin::MathSinh, Float, 1, 1},       {"cosh", Builtin::MathCosh, Float, 1, 1},
      {"tanh", Builtin::MathTanh, Float, 1, 1},       {"asinh", Builtin::MathAsinh, Float, 1, 1},
      {"acosh", Builtin::MathAcosh, Float, 1, 1},     {"atanh", Builtin::MathAtanh, Float, 1, 1},
      {"hypot", Builtin::MathHypot, Float, 0, kAny},  {"fround", Builtin::MathFround, Float, 1, 1},
      {"clz32", Builtin::MathClz32, Int32, 1, 1},     {"imul", Builtin::MathImul, Int32, 2, 2},
      {"random", Builtin::MathRandom, Float, 0, 0},
  };
  const Function *fn = nullptr;
  for (const Function &f : functions)
    if (member->name == f.name)
      fn = &f;
  if (!fn) {
    if (globalConstant("Math", member->name, member->loc()))
      diags_.error(member->loc(), "'Math." + member->name + "' is a number, not a function");
    else
      diags_.error(member->loc(), "property '" + member->name + "' does not exist on 'Math'");
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    return call;
  }
  if (fn->kind != MinMax && rejectSpread(call))
    return call;
  if (!checkArity(call, member->loc(), fn->minArgs, fn->maxArgs))
    return call;

  if (fn->kind == Pow) {
    // Math.pow(x, y) is x ** y.
    auto *pow = ctx_.make<BinaryExpr>(call->loc(), BinaryOp::Pow, call->args[0], call->args[1]);
    return checkBinary(pow, expected, /*wraps=*/false);
  }

  call->builtin = fn->builtin;
  const Type *hint = expected && expected->nonOptional()->isNumeric() ? expected->nonOptional() : nullptr;
  if (call->hasSpread())
    return checkMinMaxSpread(call, hint);
  bool ok = true;
  for (Expr *&arg : call->args) {
    arg = numericArg(arg, fn->kind == Same || fn->kind == MinMax ? hint : nullptr);
    ok &= arg->type->isNumeric();
  }
  if (!ok)
    return call;

  switch (fn->kind) {
  case Rounding:
    call->type = wholeNumberType(call->args[0]->type, expected);
    break;
  case Float:
    for (Expr *&arg : call->args)
      arg = convert(arg, Type::doubleTy());
    call->type = Type::doubleTy();
    break;
  case Same:
    call->type = call->args[0]->type;
    break;
  case MinMax: {
    // Math.min() is Infinity, a double.
    const Type *common = call->args.empty() ? Type::doubleTy() : call->args[0]->type;
    for (Expr *arg : call->args)
      common = commonNumericType(common, arg->type);
    for (Expr *&arg : call->args)
      arg = convert(arg, common);
    call->type = common;
    break;
  }
  case Int32:
    for (Expr *&arg : call->args)
      arg = toInt32(arg);
    call->type = Type::intTy();
    break;
  case Pow:
    break;
  }
  return call;
}

// `Math.max(...a, x)`: the elements of a count as arguments. Of an empty
// array Math.max gives -Infinity, which only a double holds, so the result is
// an integer only if some argument is surely there.
Expr *Sema::checkMinMaxSpread(CallExpr *call, const Type *hint) {
  const Type *common = nullptr;
  bool someValue = false, ok = true;
  for (size_t i = 0; i < call->args.size(); ++i) {
    Expr *&arg = call->args[i];
    const Type *type;
    if (call->spreads(i)) {
      markFused(arg);
      arg = iterableOf(checkExpr(arg, hint ? Type::array(hint) : nullptr));
      // The values of an iterator, a Set or a Map's values(), as an array.
      if (arg->type->isIterator() && checkSendsNothing(arg->type, arg->loc(), "a spread")) {
        auto *values = ctx_.make<ArrayLitExpr>(arg->loc());
        values->elements.push_back({arg, /*spread=*/true});
        values->type = Type::array(arg->type->yielded());
        calledFunction(); // a generator runs
        arg = values;
      }
      type = arg->type->isArray() ? arg->type->inner() : arg->type;
      if (!arg->type->isError() && (!arg->type->isArray() || !type->isNumeric())) {
        diags_.error(arg->loc(), "only an array of numbers can be spread here, found '" +
                                     arg->type->name() + "'");
        ok = false;
      }
      someValue |= provesNotEmpty(arg);
    } else {
      arg = numericArg(arg, hint);
      type = arg->type;
      someValue = true;
    }
    ok &= type->isNumeric();
    if (ok)
      common = common ? commonNumericType(common, type) : type;
  }
  if (!ok)
    return call;
  if (common->isInteger() && !someValue) {
    if (hint && hint->isInteger()) {
      auto *member = cast<MemberExpr>(call->callee);
      diags_.error(call->loc(), "Math." + member->name + " of an array that may be empty is " +
                                    (member->name == "max" ? "-" : "") +
                                    "Infinity, which only a double holds; check that the array "
                                    "is not empty, or pass a first value: 'Math." +
                                    member->name + "(0, ...a)'");
      return call;
    }
    common = Type::doubleTy();
  }
  for (size_t i = 0; i < call->args.size(); ++i)
    if (!call->spreads(i))
      call->args[i] = convert(call->args[i], common);
  call->type = common;
  return call;
}

// String(x), Number(x), parseInt(s), parseFloat(s), isNaN(x), isFinite(x),
// decimal(x).
Expr *Sema::checkGlobalCall(CallExpr *call, NameExpr *name, const Type *expected) {
  static const llvm::StringLiteral names[] = {"String",   "Number",  "parseInt", "parseFloat",
                                              "isNaN",    "isFinite", "Boolean", "decimal",
                                              "BigInt"};
  if (isGlobalName(name, "RegExp")) // `RegExp(s)` is `new RegExp(s)`
    return rejectSpread(call) ? call : checkRegExpConstructor(name->loc(), call->args);
  if (isGlobalName(name, "Date"))
    return checkDateFunction(call, name);
  if (!llvm::any_of(names, [&](llvm::StringRef n) { return isGlobalName(name, n); }))
    return nullptr;
  return checkGlobalFunction(call, name->name, name->loc(), expected);
}

Expr *Sema::checkGlobalFunction(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc,
                                const Type *expected) {
  call->type = Type::error();
  if (rejectSpread(call))
    return call;
  if (fn == "decimal")
    return checkDecimalCall(call, loc);
  if (fn == "BigInt") {
    reportBigInt(loc, "'BigInt'");
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    return call;
  }
  if (fn == "Boolean") {
    // The truth of the value, as a condition tests it; `Boolean()` is false.
    if (!checkArity(call, loc, 0, 1))
      return call;
    if (call->args.empty()) {
      auto *no = ctx_.make<BoolLitExpr>(call->loc(), false);
      no->type = Type::boolTy();
      return no;
    }
    Flow whenTrue, whenFalse;
    Expr *truth = checkBooleanCall(call, whenTrue, whenFalse);
    flow_ = join(whenTrue, whenFalse);
    return truth;
  }
  if (fn == "String" || fn == "Number") {
    if (!checkArity(call, loc, 0, 1))
      return call;
    if (fn == "String") {
      for (Expr *&arg : call->args)
        arg = toStringOperand(checkExpr(arg));
      call->builtin = Builtin::StringOf;
      call->type = Type::string();
      return call;
    }
    // Number(x) of a number, a boolean, a string, a decimal (the nearest
    // double) or undefined.
    for (Expr *&arg : call->args) {
      arg = checkExpr(arg);
      const Type *type = arg->type, *base = type->nonOptional();
      if (type->isError())
        return call;
      if (type->isDate()) // its time
        return dateTime(arg);
      if (base->isNumeric()) {
        arg = convert(arg, type->isOptional() ? Type::optional(Type::doubleTy()) : Type::doubleTy());
      } else if (!base->isBool() && !base->isString() && !base->isDecimal() &&
                 !type->isUndefined()) {
        diags_.error(arg->loc(), "Number() takes a number, a boolean, a string or a decimal, "
                                 "found '" + type->name() + "'");
        return call;
      }
    }
    call->builtin = Builtin::NumberOf;
    call->type = Type::doubleTy();
    return call;
  }

  if (fn == "parseInt" || fn == "parseFloat") {
    bool isInt = fn == "parseInt";
    if (!checkArity(call, loc, 1, isInt ? 2 : 1))
      return call;
    call->args[0] = checkArg(call->args[0], Type::string());
    if (call->args.size() == 2)
      call->args[1] = toInt32(numericArg(call->args[1]));
    call->builtin = isInt ? Builtin::ParseInt : Builtin::ParseFloat;
    // parseInt gives a whole number (or NaN), so an int where one is expected.
    call->type = isInt ? wholeNumberType(Type::doubleTy(), expected) : Type::doubleTy();
    return call;
  }

  // isNaN, isFinite
  if (!checkArity(call, loc, 1, 1))
    return call;
  call->args[0] = numericArg(call->args[0]);
  call->builtin = fn == "isNaN" ? Builtin::IsNaN : Builtin::IsFinite;
  call->type = Type::boolTy();
  return call;
}

// String.fromCharCode, String.fromCodePoint, and the functions of Number.
Expr *Sema::checkStaticCall(CallExpr *call, MemberExpr *member, const Type *expected) {
  bool isString = isGlobalName(member->object, "String");
  llvm::StringRef fn = member->name;
  call->type = Type::error();
  if (isString && fn == "raw") {
    // String.raw`C:\dir` is the template itself, with its texts as written
    // (see Parser::parseCallOrMember).
    if (!call->tagged) {
      diags_.error(member->loc(), "String.raw can only tag a template, e.g. String.raw`C:\\dir`");
      for (Expr *&arg : call->args)
        arg = checkExpr(arg);
      return call;
    }
    auto *texts = cast<ArrayLitExpr>(call->args[0]);
    auto *tmpl = ctx_.make<TemplateExpr>(texts->loc());
    for (const ArrayLitExpr::Element &e : texts->elements)
      tmpl->quasis.push_back(cast<StringLitExpr>(e.expr)->value);
    tmpl->exprs.assign(call->args.begin() + 1, call->args.end());
    return checkTemplate(tmpl);
  }
  if (isString && (fn == "fromCharCode" || fn == "fromCodePoint")) {
    bool codeUnits = fn == "fromCharCode";
    for (Expr *&arg : call->args) {
      // Code units are taken modulo 2^16, as JavaScript does; a code point
      // must be one, or the program stops with a RangeError.
      arg = numericArg(arg);
      arg = codeUnits ? toInt32(arg) : convert(arg, Type::doubleTy());
    }
    call->builtin = codeUnits ? Builtin::FromCharCode : Builtin::FromCodePoint;
    call->type = Type::string();
    return call;
  }
  if (!isString && (fn == "parseInt" || fn == "parseFloat"))
    return checkGlobalFunction(call, fn, member->loc(), expected); // Number.parseInt is parseInt
  static const std::pair<llvm::StringLiteral, Builtin> predicates[] = {
      {"isNaN", Builtin::IsNaN},
      {"isFinite", Builtin::IsFinite},
      {"isInteger", Builtin::IsInteger},
      {"isSafeInteger", Builtin::IsSafeInteger},
  };
  if (!isString) {
    for (const auto &[name, builtin] : predicates) {
      if (fn != name)
        continue;
      if (!checkArity(call, member->loc(), 1, 1))
        return call;
      call->args[0] = numericArg(call->args[0]);
      call->builtin = builtin;
      call->type = Type::boolTy();
      return call;
    }
  }
  diags_.error(member->loc(), "property '" + fn + "' does not exist on '" +
                                  (isString ? "String" : "Number") + "'");
  for (Expr *&arg : call->args)
    arg = checkExpr(arg);
  return call;
}

// x.toString() and x.toFixed(digits) of a number.
Expr *Sema::checkNumberMethod(CallExpr *call, MemberExpr *member) {
  if (member->name == "toString") {
    if (!checkArity(call, member->loc(), 0, 0))
      return call;
    // `x.toString()` is `String(x)`.
    call->args.push_back(toStringOperand(member->object));
    call->builtin = Builtin::StringOf;
    call->type = Type::string();
    return call;
  }
  if (member->name == "toFixed") {
    if (!checkArity(call, member->loc(), 0, 1))
      return call;
    member->object = convert(member->object, Type::doubleTy());
    if (!call->args.empty())
      call->args[0] = checkPosition(call->args[0]);
    call->builtin = Builtin::NumToFixed;
    call->type = Type::string();
    return call;
  }
  diags_.error(member->loc(), "property '" + member->name + "' does not exist on type '" +
                                  member->object->type->name() + "'");
  for (Expr *&arg : call->args)
    arg = checkExpr(arg);
  return call;
}

Expr *Sema::checkStringMethod(CallExpr *call, MemberExpr *member) {
  if (Expr *regex = checkStringRegexMethod(call, member))
    return regex;
  struct Method {
    llvm::StringRef name;
    Builtin builtin;
    unsigned minArgs, maxArgs;
    // The kinds of the arguments: 's' a string, 'p' a position or count,
    // '*' any value, turned into a string. The last one repeats.
    const char *args;
    const Type *ret;
  };
  constexpr unsigned kAny = ~0u;
  static const Method methods[] = {
      {"charCodeAt", Builtin::StrCharCodeAt, 1, 1, "p", Type::intTy()},
      {"charAt", Builtin::StrCharAt, 0, 1, "p", Type::string()},
      {"codePointAt", Builtin::StrCodePointAt, 1, 1, "p", Type::intTy()},
      {"at", Builtin::StrAt, 1, 1, "p", Type::string()},
      {"slice", Builtin::StrSlice, 0, 2, "p", Type::string()},
      {"substring", Builtin::StrSubstring, 1, 2, "p", Type::string()},
      {"indexOf", Builtin::StrIndexOf, 1, 2, "sp", Type::intTy()},
      {"lastIndexOf", Builtin::StrLastIndexOf, 1, 2, "sp", Type::intTy()},
      {"includes", Builtin::StrIncludes, 1, 2, "sp", Type::boolTy()},
      {"startsWith", Builtin::StrStartsWith, 1, 2, "sp", Type::boolTy()},
      {"endsWith", Builtin::StrEndsWith, 1, 2, "sp", Type::boolTy()},
      {"repeat", Builtin::StrRepeat, 1, 1, "p", Type::string()},
      {"toString", Builtin::StrToString, 0, 0, "", Type::string()},
      {"concat", Builtin::StrConcat, 0, kAny, "*", Type::string()},
      {"split", Builtin::StrSplit, 0, 2, "sp", Type::array(Type::string())},
      {"trim", Builtin::StrTrim, 0, 0, "", Type::string()},
      {"trimStart", Builtin::StrTrimStart, 0, 0, "", Type::string()},
      {"trimEnd", Builtin::StrTrimEnd, 0, 0, "", Type::string()},
      {"padStart", Builtin::StrPadStart, 1, 2, "ps", Type::string()},
      {"padEnd", Builtin::StrPadEnd, 1, 2, "ps", Type::string()},
      {"toUpperCase", Builtin::StrToUpperCase, 0, 0, "", Type::string()},
      {"toLowerCase", Builtin::StrToLowerCase, 0, 0, "", Type::string()},
      {"replace", Builtin::StrReplace, 2, 2, "ss", Type::string()},
      {"replaceAll", Builtin::StrReplaceAll, 2, 2, "ss", Type::string()},
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
  if (!checkArity(call, member->loc(), method->minArgs, method->maxArgs))
    return call;

  size_t kinds = std::strlen(method->args);
  for (size_t i = 0; i < call->args.size(); ++i) {
    Expr *&arg = call->args[i];
    switch (method->args[std::min(i, kinds - 1)]) {
    case 's':
      arg = checkArg(arg, Type::string());
      break;
    case 'p':
      arg = checkPosition(arg);
      break;
    default:
      arg = toStringOperand(checkExpr(arg));
      break;
    }
  }
  call->builtin = method->builtin;
  call->type = method->ret;

  if (method->builtin == Builtin::StrCharCodeAt) {
    // Out of range, JavaScript returns NaN, which only a double can hold.
    call->mayBeOutOfRange = !provesInBounds(member->object, call->args[0], /*allowEnd=*/false);
    call->type = call->mayBeOutOfRange ? Type::doubleTy() : Type::intTy();
  } else if (method->builtin == Builtin::StrCodePointAt) {
    // Out of range, JavaScript returns undefined.
    call->mayBeOutOfRange = !provesInBounds(member->object, call->args[0], /*allowEnd=*/false);
    call->type = call->mayBeOutOfRange ? Type::optional(Type::intTy()) : Type::intTy();
  } else if (method->builtin == Builtin::StrAt) {
    call->mayBeOutOfRange = !provesAtInBounds(member->object, call->args[0]);
    call->type = call->mayBeOutOfRange ? Type::optional(Type::string()) : Type::string();
  }
  return call;
}

Expr *Sema::checkArrayMethod(CallExpr *call, MemberExpr *member, const Type *expected) {
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
  if (llvm::is_contained(callbacks, name) || (name == "sort" && count != 0))
    return checkCallbackMethod(call, member, expected);
  if (name == "keys" || name == "values" || name == "entries")
    return checkArrayIterator(call, member);

  if (name == "push" || name == "unshift") {
    bool spreads = call->hasSpread();
    for (size_t i = 0; i < count; ++i) {
      if (call->spreads(i)) // `a.push(...b)`: b has the same type as a
        call->args[i] = coerce(checkExpr(call->args[i], arrayType), arrayType);
      else
        elementArg(i);
    }
    call->builtin = name == "push" ? Builtin::ArrPush : Builtin::ArrUnshift;
    call->type = Type::intTy();
    // The length grows by exactly `count`, unless arrays are spread.
    if (self && !spreads)
      flow_.facts.shift(Term::lengthOf(self), static_cast<int64_t>(count));
    arrayResized(arrayType, /*grew=*/true, /*shrank=*/false, spreads ? nullptr : self);
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
    if (holdsResults(element)) {
      diags_.error(member->loc(), "'" + name +
                                      "' cannot compare Results: in JavaScript each Result is an "
                                      "object of its own, equal to no other; search with a "
                                      "function, e.g. 'some' or 'findIndex'");
      checkArgsLoosely();
      return call;
    }
    if (element->nonOptional()->isFunction())
      file_->comparesFunctions = true;
    elementArg(0);
    positionArgs(1);
    call->builtin = name == "indexOf"       ? Builtin::ArrIndexOf
                    : name == "lastIndexOf" ? Builtin::ArrLastIndexOf
                                            : Builtin::ArrIncludes;
    call->type = name == "includes" ? Type::boolTy() : Type::intTy();
    return call;
  }

  if ((name == "join" || name == "toString") && holdsFunctions(element)) {
    diags_.error(member->loc(), "functions cannot be converted to strings");
    checkArgsLoosely();
    return call;
  }
  if ((name == "join" || name == "toString") && holdsResults(element)) {
    diags_.error(member->loc(), "Results cannot be converted to strings: JavaScript would give "
                                "'[object Object]'; make the text of each first, e.g. 'a.map(r => "
                                "r.match(...))." +
                                    name + "()'");
    checkArgsLoosely();
    return call;
  }
  if ((name == "join" || name == "toString") && holdsUnknown(element)) {
    diags_.error(member->loc(), "unknown values cannot be converted to strings: one may hold an "
                                "object; map them to text first, e.g. 'a.map(x => typeof x == "
                                "\"string\" ? x : \"?\")." +
                                    name + "()'");
    checkArgsLoosely();
    return call;
  }
  if ((name == "join" || name == "toString") && holdsJson(element)) {
    diags_.error(member->loc(), "Json values cannot be converted to strings: an object would be "
                                "'[object Object]'; map them to text first, e.g. 'a.map(x => "
                                "JSON.stringify(x))." +
                                    name + "()'");
    checkArgsLoosely();
    return call;
  }
  if ((name == "join" || name == "toString") && holdsObjects(element) && !holdsDates(element)) {
    diags_.error(member->loc(), "objects cannot be converted to strings: JavaScript would give "
                                "'[object Object]'; map them to text first, e.g. 'a.map(o => o.name)." +
                                    name + "()'");
    checkArgsLoosely();
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
    if (holdsFunctions(element)) {
      diags_.error(member->loc(), "sorting functions needs a comparison function: 'sort()' "
                                  "without one compares their source code");
      return call;
    }
    if (holdsUnknown(element)) {
      diags_.error(member->loc(), "sorting unknown values needs a comparison function: 'sort()' "
                                  "without one compares them as strings, objects as '[object "
                                  "Object]'; pass one, e.g. 'sort((a, b) => ...)'");
      return call;
    }
    if (holdsJson(element)) {
      diags_.error(member->loc(), "sorting Json values needs a comparison function: 'sort()' "
                                  "without one compares them as strings, objects as '[object "
                                  "Object]'; pass one, e.g. 'sort((a, b) => ...)'");
      return call;
    }
    if (holdsDates(element)) {
      diags_.error(member->loc(), "sorting dates needs a comparison function: 'sort()' without one "
                                  "compares the texts of toString(), 'Thu Feb 29 2024 ...', which "
                                  "puts them in the order of the names of the days; pass one: "
                                  "'sort((a, b) => a.getTime() - b.getTime())'");
      return call;
    }
    if (holdsObjects(element) || holdsResults(element)) {
      diags_.error(member->loc(), "sorting objects needs a comparison function: 'sort()' without one "
                                  "compares them all as '[object Object]'; pass one, e.g. 'sort((a, b) "
                                  "=> a.x - b.x)'");
      return call;
    }
    // A union compares its values as strings too, numbers and objects among
    // them: the same traps.
    auto textual = [](const Type *m) {
      const Type *b = m->isLiteral() ? m->literalBase() : m;
      return b->isString() || b->isBool();
    };
    if (const Type *u = element->nonOptional();
        u->isUnion() && !llvm::all_of(u->members(), textual)) {
      diags_.error(member->loc(), "'sort()' without a comparison function compares the values "
                                  "of '" + u->name() + "' as strings, numbers and objects among "
                                  "them; pass one, e.g. 'sort((a, b) => ...)'");
      return call;
    }
    // JavaScript would sort numbers as strings; sorting them differently
    // would give another order than node for the same code. (Undefined
    // elements go to the end without being compared.)
    if (element->nonOptional()->isNumeric() || element->nonOptional()->isDecimal() ||
        apparentType(element->nonOptional())->isNumeric()) {
      diags_.error(member->loc(), llvm::Twine("'sort()' without a comparison function compares "
                                              "numbers as strings, as in JavaScript: [2, 10, 1] "
                                              "becomes [1, 10, 2]; pass one: 'sort((x, y) => ") +
                                      (element->isOptional() ? "(x ?? 0) - (y ?? 0)" : "x - y") + ")'");
      return call;
    }
    call->builtin = Builtin::ArrSort;
    call->type = arrayType;
    return call;
  }

  if (name == "splice") {
    // splice(start, deleteCount, ...items): removes and returns elements,
    // and inserts items in their place.
    if (!arity(1, ~0u))
      return call;
    for (size_t i = 0; i < count; ++i) {
      if (i < 2)
        call->args[i] = checkPosition(call->args[i]);
      else
        elementArg(i);
    }
    call->builtin = Builtin::ArrSplice;
    call->type = arrayType;
    arrayResized(arrayType, /*grew=*/true, /*shrank=*/true);
    return call;
  }
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
  // `new cls()` and `new route.cls()` of a variable that holds a class
  // (docs/decorators.md, sections 5 and 6).
  if (!expr->callee && !expr->name.empty() && lookupValue(expr->name).var) {
    expr->callee = ctx_.make<NameExpr>(expr->loc(), expr->name);
    if (!expr->member.empty())
      expr->callee = ctx_.make<MemberExpr>(expr->loc(), expr->callee, expr->member);
  }
  if (expr->callee)
    return checkValueNew(expr, expected);
  if (!expr->member.empty()) {
    ScopeEntry intl = lookup("Intl");
    if (expr->name == "Intl" && !intl.var && !intl.func && !intl.cls)
      return expr->member == "DateTimeFormat"
                 ? checkDateFormatNew(expr->loc(), expr->args)
                 : checkIntlNew(expr->loc(), expr->member, expr->args, /*withNew=*/true);
    diags_.error(expr->loc(), "'" + expr->name + "." + expr->member +
                                  "' is not supported; of namespaces, only the classes of 'Intl' "
                                  "can be made with 'new'");
    checkArgsLoosely();
    return expr;
  }
  if (ClassDecl *cls = lookupValue(expr->name).cls)
    return checkClassNew(expr, finalClass(cls), expected);
  if (const NodeName *n = nodeName(expr->name); n && n->module == NodeModule::Crypto)
    return checkCryptoNew(expr, n->name);
  if (const char *why = whyUnsupported(expr->name)) {
    diags_.error(expr->loc(), "'new " + expr->name + "' is not supported: " + why);
    checkArgsLoosely();
    return expr;
  }
  if (expr->name == "RegExp" && !lookup("RegExp").var && !lookup("RegExp").func) {
    if (!expr->typeArgs.empty())
      diags_.error(expr->loc(), "'RegExp' takes no type arguments");
    return checkRegExpConstructor(expr->loc(), expr->args);
  }
  if (expr->name == "Date" && !lookup("Date").var && !lookup("Date").func) {
    if (!expr->typeArgs.empty())
      diags_.error(expr->loc(), "'Date' takes no type arguments");
    return checkDateNew(expr->loc(), expr->args);
  }
  if ((expr->name == "Map" || expr->name == "Set" || expr->name == "WeakMap" ||
       expr->name == "WeakSet") &&
      !lookup(expr->name).var && !lookup(expr->name).func)
    return checkDictNew(expr, expected);
  if (expr->name == "Promise" && !lookup("Promise").var && !lookup("Promise").func)
    return checkPromiseNew(expr, expected);
  if (expr->name == "WeakRef" && !lookup("WeakRef").var && !lookup("WeakRef").func)
    return checkWeakRefNew(expr, expected);
  if (const Type *bytes = bytesTypeNamed(expr->name);
      bytes && !lookup(expr->name).var && !lookup(expr->name).func)
    return checkBytesNew(expr, bytes);
  if (expr->name == "BigInt64Array" || expr->name == "BigUint64Array") {
    reportBigInt(expr->loc(), "'" + expr->name.str() + "'");
    checkArgsLoosely();
    return expr;
  }
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
  if (expected && expected->nonOptional()->isJson())
    return checkJsonArrayLit(lit);
  // Where an unknown is expected, the elements may be of any types too.
  if (expected && expected->isUnknown())
    expected = Type::array(Type::unknown());
  const Type *expectedElement = nullptr;
  // Where a union is expected, its array, if it has one: `string | double[]`.
  if (expected && expected->nonOptional()->isUnion())
    if (const Type *array = expected->nonOptional()->slotPart(UnionSlot::Array);
        array && array->isArray())
      expected = array;
  if (expected && expected->nonOptional()->isArray())
    expectedElement = expected->nonOptional()->inner();

  if (auto hole = llvm::find_if(lit->elements, [](const ArrayLitExpr::Element &e) { return e.hole; });
      hole != lit->elements.end()) {
    diags_.error(hole->expr->loc(), "arrays cannot have holes; write the element explicitly");
    for (ArrayLitExpr::Element &e : lit->elements)
      e.expr = checkExpr(e.expr);
    lit->type = Type::error();
    return lit;
  }
  if (expected && expected->nonOptional()->isTuple())
    return checkTupleLit(lit, expected->nonOptional());

  // The element types: of the values, or of the arrays spread into this one.
  // A string spreads into its characters, a tuple into its elements. Literal
  // numbers next to a decimal element are decimals: they are checked after
  // the other elements, which changes nothing else.
  std::vector<const Type *> types(lit->elements.size());
  bool someDecimal = false;
  const Type *seen = nullptr; // the type of the elements so far
  for (size_t i = 0; i < lit->elements.size(); ++i) {
    ArrayLitExpr::Element &e = lit->elements[i];
    if (!e.spread && !expectedElement && isLiteralTree(e.expr))
      continue;
    if (!e.spread) {
      // `[p, { x: 1, y: 2 }]`: a literal takes the type of the objects
      // before it.
      const Type *hint = expectedElement;
      if (!hint && seen && holdsObjects(seen) && literalShaped(e.expr))
        hint = seen;
      e.expr = checkExpr(e.expr, hint);
      types[i] = e.expr->type;
      someDecimal |= types[i]->nonOptional()->isDecimal();
      if (!e.expr->type->isError() && seen != Type::error())
        seen = seen ? unify(seen, e.expr->type) : e.expr->type;
      if (!seen)
        seen = Type::error();
      continue;
    }
    markFused(e.expr);
    e.expr =
        iterableOf(checkExpr(e.expr, expectedElement ? Type::array(expectedElement) : nullptr));
    if (e.expr->type->isMatch()) // `[...m]`: the captures
      e.expr = matchItems(e.expr);
    const Type *type = e.expr->type;
    if (type->isString()) {
      types[i] = type;
      continue;
    }
    // `[...gen()]`: the values of the iterator, one after another.
    if (type->isIterator()) {
      checkSendsNothing(type, e.expr->loc(), "a spread");
      calledFunction(); // a generator runs
      types[i] = type->yielded();
      someDecimal |= types[i]->nonOptional()->isDecimal();
      continue;
    }
    if (type->isTuple()) {
      const Type *element = tupleElement(type);
      if (!element)
        diags_.error(e.expr->loc(),
                     "the elements of '" + type->name() +
                         "' have different types, and an array holds elements of one type");
      types[i] = element ? element : Type::error();
      continue;
    }
    // `[...bytes]`: the elements as numbers.
    if (type->isTypedArray()) {
      types[i] = type->elementType();
      continue;
    }
    if (!type->isArray() && !type->isError()) {
      diags_.error(e.expr->loc(), "only arrays, strings, iterators, Maps and Sets can be spread "
                                  "here, found '" +
                                      type->name() + "'" +
                                      (type->isRecord() ? "; for the entries of a Record, spread "
                                                          "'Object.entries(r)'"
                                                        : ""));
      type = Type::error();
    }
    types[i] = type->isError() ? type : type->inner();
    someDecimal |= types[i]->nonOptional()->isDecimal();
  }
  for (size_t i = 0; i < lit->elements.size(); ++i) {
    ArrayLitExpr::Element &e = lit->elements[i];
    if (types[i])
      continue;
    e.expr = checkExpr(e.expr, someDecimal ? Type::decimalTy() : nullptr);
    types[i] = e.expr->type;
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
      const Type *next = element ? unionUnify(element, types[i]) : types[i];
      if (!next) {
        std::string message = "array elements must have one type; found '" + element->name().str() +
                              "' and '" + types[i]->name().str() + "'";
        // A value of each type in its own place is a tuple, which must be
        // expected there: JavaScript makes an array.
        bool spreads =
            llvm::any_of(lit->elements, [](const ArrayLitExpr::Element &e) { return e.spread; });
        auto literalClass = [](const Type *t) {
          const ClassDecl *cls = t->nonOptional()->classDecl();
          return cls && cls->literal;
        };
        if (literalClass(element) && literalClass(types[i])) {
          message += "; each object literal with methods has a type of its own, so objects made "
                     "by different literals differ; an interface with their fields and methods "
                     "joins them: 'const xs: Counter[] = [...]'";
        } else if (!spreads && llvm::none_of(types, [](const Type *t) { return t->isError(); })) {
          std::string tuple;
          for (const Type *t : types)
            tuple += (tuple.empty() ? "" : ", ") + t->name().str();
          message += "; for a value of each type in its own place, declare a tuple, e.g. "
                     "'const t: [" +
                     tuple + "] = ...'";
        }
        diags_.error(lit->elements[i].expr->loc(), message);
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
    } else if (e.expr->type != arrayType && !e.expr->type->isError() &&
               !(e.expr->type->isString() && element->isString()) &&
               // `[...dogs]` as Animal[]: the same references, copied; also of
               // a union of objects, which is one reference too.
               !(e.expr->type->isArray() && isObjectReference(e.expr->type->inner()->nonOptional()) &&
                 element->nonOptional()->isObject() &&
                 (e.expr->type->inner()->isOptional() == element->isOptional() ||
                  element->isOptional()) &&
                 isImplicitlyConvertible(e.expr->type->inner(), element)) &&
               !(e.expr->type->isIterator() &&
                 isImplicitlyConvertible(e.expr->type->yielded(), element)) &&
               !(e.expr->type->isTypedArray() &&
                 isImplicitlyConvertible(e.expr->type->elementType(), element)) &&
               !(e.expr->type->isTuple() &&
                 llvm::all_of(e.expr->type->fields(), [&](const Field &f) {
                   return isImplicitlyConvertible(f.type, element);
                 }))) {
      // Arrays of different types have different layouts, so no conversion.
      diags_.error(e.expr->loc(), "cannot spread '" + e.expr->type->name() + "' into '" +
                                      arrayType->name() + "'");
    }
  }
  lit->type = arrayType;
  return lit;
}

} // namespace tz
