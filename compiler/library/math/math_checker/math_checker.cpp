#include "compiler/library/math/math_checker/math_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/binary_checker/binary_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/declare/constant_folder/constant_folder.h"
#include "compiler/check/objects/dict_checker/dict_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/flow/index_proofs/index_proofs.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/iterators/generator_checker/generator_checker.h"
#include "compiler/check/iterators/iterator_methods/iterator_methods.h"
#include "llvm/ADT/STLExtras.h"
#include <cstring>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

// The type a call that gives a whole number has where `expected` is
// expected: an int or a long when one is expected, which the program checks
// the number fits in (JavaScript would go on with a number that type cannot
// hold), and otherwise `type` itself.
const Type *wholeNumberType(const Type *type, const Type *expected) {
  const Type *wanted = expected ? expected->nonOptional() : nullptr;
  bool narrows =
      wanted && wanted->isInteger() && (type->isFloat() || type->bitWidth() > wanted->bitWidth());

  return narrows ? wanted : type;
}

} // namespace

MathChecker::MathChecker(Sema &sema) : sema_(sema) {}

Expr *MathChecker::numericArg(Expr *arg, const Type *hint) {
  arg = sema_.genericChecker().apparent(sema_.exprChecker().checkExpr(arg, hint));
  const Type *type = arg->type;
  if (!type->isNumeric() && !type->isError()) {
    std::string message = "argument of type '" + type->name().str() +
                          "' is not assignable to parameter of type 'double'";
    if (type->isOptional() && type->inner()->isNumeric()) {
      message += "; the value may be undefined: check it first or give a default with '??'";
    } else if (type->isDecimal()) {
      message += "; convert it with 'Number(x)', or use what decimals have: 'x.abs()', 'x.sqrt()', "
                 "'x.round()', 'decimal.max(a, b)'...";
    }
    sema_.diags_.error(arg->loc(), message);
  }

  return arg;
}

// The functions of Math, which work as in JavaScript. Most take and return
// doubles. Others keep the type of their arguments: abs, sign, min and max
// of ints give an int, and pow is `**`. floor, ceil, round and trunc give a
// double, or an int or a long where one is expected (see wholeNumberType).
Expr *MathChecker::checkMathCall(CallExpr *call, MemberExpr *member, const Type *expected) {
  enum Kind {
    Rounding, // floor, ceil, round, trunc
    Float,    // doubles in, a double out
    Same,     // the type of the argument out: abs, sign
    MinMax,   // the common type of the arguments
    Pow,
    Int32, // ToInt32 of the arguments, an int out: clz32, imul
  };
  struct Function {
    llvm::StringLiteral name;
    Builtin builtin;
    Kind kind;
    unsigned minArgs, maxArgs;
  };
  constexpr unsigned kAny = ~0u;
  static const Function functions[] = {
      {"floor", Builtin::MathFloor, Rounding, 1, 1}, {"ceil", Builtin::MathCeil, Rounding, 1, 1},
      {"round", Builtin::MathRound, Rounding, 1, 1}, {"trunc", Builtin::MathTrunc, Rounding, 1, 1},
      {"abs", Builtin::MathAbs, Same, 1, 1},         {"sign", Builtin::MathSign, Same, 1, 1},
      {"min", Builtin::MathMin, MinMax, 0, kAny},    {"max", Builtin::MathMax, MinMax, 0, kAny},
      {"pow", Builtin::MathPow, Pow, 2, 2},          {"sqrt", Builtin::MathSqrt, Float, 1, 1},
      {"cbrt", Builtin::MathCbrt, Float, 1, 1},      {"exp", Builtin::MathExp, Float, 1, 1},
      {"expm1", Builtin::MathExpm1, Float, 1, 1},    {"log", Builtin::MathLog, Float, 1, 1},
      {"log1p", Builtin::MathLog1p, Float, 1, 1},    {"log2", Builtin::MathLog2, Float, 1, 1},
      {"log10", Builtin::MathLog10, Float, 1, 1},    {"sin", Builtin::MathSin, Float, 1, 1},
      {"cos", Builtin::MathCos, Float, 1, 1},        {"tan", Builtin::MathTan, Float, 1, 1},
      {"asin", Builtin::MathAsin, Float, 1, 1},      {"acos", Builtin::MathAcos, Float, 1, 1},
      {"atan", Builtin::MathAtan, Float, 1, 1},      {"atan2", Builtin::MathAtan2, Float, 2, 2},
      {"sinh", Builtin::MathSinh, Float, 1, 1},      {"cosh", Builtin::MathCosh, Float, 1, 1},
      {"tanh", Builtin::MathTanh, Float, 1, 1},      {"asinh", Builtin::MathAsinh, Float, 1, 1},
      {"acosh", Builtin::MathAcosh, Float, 1, 1},    {"atanh", Builtin::MathAtanh, Float, 1, 1},
      {"hypot", Builtin::MathHypot, Float, 0, kAny}, {"fround", Builtin::MathFround, Float, 1, 1},
      {"clz32", Builtin::MathClz32, Int32, 1, 1},    {"imul", Builtin::MathImul, Int32, 2, 2},
      {"random", Builtin::MathRandom, Float, 0, 0},
  };
  const Function *fn = nullptr;
  for (const Function &f : functions) {
    if (member->name == f.name) {
      fn = &f;
    }
  }
  if (!fn) {
    if (sema_.constantFolder().globalConstant("Math", member->name, member->loc())) {
      sema_.diags_.error(member->loc(), "'Math." + member->name + "' is a number, not a function");
    } else {
      sema_.diags_.error(member->loc(), "property '" + member->name + "' does not exist on 'Math'");
    }
    for (Expr *&arg : call->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }
    return call;
  }
  if (fn->kind != MinMax && sema_.callChecker().rejectSpread(call)) {
    return call;
  }
  if (!sema_.argumentChecker().checkArity(call, member->loc(), fn->minArgs, fn->maxArgs)) {
    return call;
  }

  if (fn->kind == Pow) {
    // Math.pow(x, y) is x ** y.
    auto *pow = sema_.ctx_.make<BinaryExpr>(call->loc(), BinaryOp::Pow, call->args[0], call->args[1]);
    return sema_.binaryChecker().checkBinary(pow, expected, /*wraps=*/false);
  }

  call->builtin = fn->builtin;
  const Type *hint =
      expected && expected->nonOptional()->isNumeric() ? expected->nonOptional() : nullptr;
  if (call->hasSpread()) {
    return checkMinMaxSpread(call, hint);
  }
  bool ok = true;
  for (Expr *&arg : call->args) {
    arg = numericArg(arg, fn->kind == Same || fn->kind == MinMax ? hint : nullptr);
    ok &= arg->type->isNumeric();
  }
  if (!ok) {
    return call;
  }

  switch (fn->kind) {
  case Rounding:
    call->type = wholeNumberType(call->args[0]->type, expected);
    break;
  case Float:
    for (Expr *&arg : call->args) {
      arg = sema_.typeRelations().convert(arg, Type::doubleTy());
    }
    call->type = Type::doubleTy();
    break;
  case Same:
    call->type = call->args[0]->type;
    break;
  case MinMax: {
    // Math.min() is Infinity, a double.
    const Type *common = call->args.empty() ? Type::doubleTy() : call->args[0]->type;
    for (Expr *arg : call->args) {
      common = TypeRelations::commonNumericType(common, arg->type);
    }
    for (Expr *&arg : call->args) {
      arg = sema_.typeRelations().convert(arg, common);
    }
    call->type = common;
    break;
  }
  case Int32:
    for (Expr *&arg : call->args) {
      arg = sema_.binaryChecker().toInt32(arg);
    }
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
Expr *MathChecker::checkMinMaxSpread(CallExpr *call, const Type *hint) {
  const Type *common = nullptr;
  bool someValue = false, ok = true;
  for (size_t i = 0; i < call->args.size(); ++i) {
    Expr *&arg = call->args[i];
    const Type *type;
    if (call->spreads(i)) {
      sema_.iteratorMethods().markFused(arg);
      arg = sema_.dictChecker().iterableOf(sema_.exprChecker().checkExpr(arg, hint ? Type::array(hint) : nullptr));
      // The values of an iterator, a Set or a Map's values(), as an array.
      if (arg->type->isIterator() && sema_.generatorChecker().checkSendsNothing(arg->type, arg->loc(), "a spread")) {
        auto *values = sema_.ctx_.make<ArrayLitExpr>(arg->loc());
        values->elements.push_back({arg, /*spread=*/true});
        values->type = Type::array(arg->type->yielded());
        sema_.flowChecker().calledFunction(); // a generator runs
        arg = values;
      }
      type = arg->type->isArray() ? arg->type->inner() : arg->type;
      if (!arg->type->isError() && (!arg->type->isArray() || !type->isNumeric())) {
        sema_.diags_.error(arg->loc(), "only an array of numbers can be spread here, found '" +
                                     arg->type->name() + "'");
        ok = false;
      }
      someValue |= sema_.indexProofs().provesNotEmpty(arg);
    } else {
      arg = numericArg(arg, hint);
      type = arg->type;
      someValue = true;
    }
    ok &= type->isNumeric();
    if (ok) {
      common = common ? TypeRelations::commonNumericType(common, type) : type;
    }
  }
  if (!ok) {
    return call;
  }
  if (common->isInteger() && !someValue) {
    if (hint && hint->isInteger()) {
      auto *member = cast<MemberExpr>(call->callee);
      sema_.diags_.error(call->loc(), "Math." + member->name + " of an array that may be empty is " +
                                    (member->name == "max" ? "-" : "") +
                                    "Infinity, which only a double holds; check that the array "
                                    "is not empty, or pass a first value: 'Math." +
                                    member->name + "(0, ...a)'");
      return call;
    }
    common = Type::doubleTy();
  }
  for (size_t i = 0; i < call->args.size(); ++i) {
    if (!call->spreads(i)) {
      call->args[i] = sema_.typeRelations().convert(call->args[i], common);
    }
  }
  call->type = common;

  return call;
}

} // namespace tz
