#include "compiler/pipeline/check_program/topics/math/math_checker/math_checker.h"
#include "compiler/pipeline/check_program/declare/constant_folder/constant_folder.h"
#include "compiler/pipeline/check_program/flow/flow_checker/flow_checker.h"
#include "compiler/pipeline/check_program/flow/index_proofs/index_proofs.h"
#include "compiler/pipeline/check_program/generics/generic_checker/generic_checker.h"
#include "compiler/pipeline/check_program/topics/expr/argument_checker/argument_checker.h"
#include "compiler/pipeline/check_program/topics/expr/binary_checker/binary_checker.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/iterators/generator_checker/generator_checker.h"
#include "compiler/pipeline/check_program/topics/iterators/iterator_methods/iterator_methods.h"
#include "compiler/pipeline/check_program/topics/objects/dict_checker/dict_checker.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include <cstring>

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

MathChecker::MathChecker(CheckState &owner)
    : state(owner) {}

Expr *MathChecker::numericArg(Expr *arg, const Type *hint) {
  arg = GenericChecker(this->state).apparent(ExprChecker(this->state).checkExpr(arg, hint));
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
    this->state.getDiagnostics().error(arg->loc(), message);
  }

  return arg;
}

Expr *MathChecker::checkMathCall(CallExpr *call, MemberExpr *member, const Type *expected) {
  enum Kind {
    Rounding,
    Float,
    Same,
    MinMax,
    Pow,
    Int32,
    Clamp,
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
      {"random", Builtin::MathRandom, Float, 0, 0},  {"clamp", Builtin::MathClamp, Clamp, 3, 3},
  };
  const Function *fn = nullptr;
  for (const Function &f : functions) {
    if (member->name == f.name) {
      fn = &f;
    }
  }
  if (!fn) {
    if (ConstantFolder(this->state).globalConstant("Math", member->name, member->loc())) {
      this->state.getDiagnostics().error(member->loc(),
                                         "'Math." + member->name + "' is a number, not a function");
    } else {
      this->state.getDiagnostics().error(member->loc(), "property '" + member->name +
                                                            "' does not exist on 'Math'");
    }
    for (Expr *&arg : call->args) {
      arg = ExprChecker(this->state).checkExpr(arg);
    }
    return call;
  }
  if (fn->kind != MinMax && CallChecker(this->state).rejectSpread(call)) {
    return call;
  }
  ArgumentChecker::ResultArityMatches arity =
      ArgumentChecker(this->state).checkArity(call, fn->minArgs, fn->maxArgs);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  if (fn->kind == MinMax && call->args.empty()) {
    this->state.getDiagnostics().error(
        call->loc(), "'Math." + member->name + "()' without values gives " +
                         (member->name == "max" ? "-" : "") +
                         "Infinity; pass the values, e.g. 'Math." + member->name + "(...xs)'");
    return call;
  }

  if (fn->kind == Pow) {
    auto *pow = this->state.getAst().make<BinaryExpr>(call->loc(), BinaryOp::Pow, call->args[0],
                                                      call->args[1]);
    return BinaryChecker(this->state).checkBinary(pow, expected, false);
  }

  call->target = BuiltinCall{.builtin = fn->builtin};
  const Type *hint =
      expected && expected->nonOptional()->isNumeric() ? expected->nonOptional() : nullptr;
  if (call->hasSpread()) {
    return this->checkMinMaxSpread(call, hint);
  }
  bool ok = true;
  for (Expr *&arg : call->args) {
    const Type *argHint =
        fn->kind == Same || fn->kind == MinMax || fn->kind == Clamp ? hint : nullptr;
    if (fn->kind == Float || fn->kind == Rounding) {
      argHint = Type::doubleTy();
    }
    arg = this->numericArg(arg, argHint);
    ok &= arg->type->isNumeric();
  }
  if (!ok) {
    return call;
  }

  switch (fn->kind) {
  case Rounding:
    if (call->args[0]->type->isInteger()) {
      this->state.getDiagnostics().error(
          call->args[0]->loc(), "'Math." + member->name + "' rounds a double, and this is '" +
                                    call->args[0]->type->name() +
                                    "', which has no fraction: '/' of integers already drops it");
      return call;
    }
    call->type = Type::doubleTy();
    break;
  case Float:
    for (Expr *&arg : call->args) {
      arg = TypeRelations(this->state).coerce(arg, Type::doubleTy());
    }
    call->type = Type::doubleTy();
    break;
  case Same:
    call->type = call->args[0]->type;
    break;
  case MinMax:
  case Clamp: {
    const Type *common = this->sharedType(call->args, {});
    if (!common) {
      return call;
    }
    call->type = common;
    break;
  }
  case Int32:
    for (Expr *&arg : call->args) {
      arg = this->bitsArg(arg, 32,
                          fn->builtin == Builtin::MathImul ? Type::integer(32, false) : nullptr,
                          "Math." + member->name.str());
      if (arg->type->isError()) {
        return call;
      }
      if (fn->builtin == Builtin::MathImul) {
        arg = TypeRelations(this->state).convert(arg, Type::integer(32, false));
      }
    }
    call->type = fn->builtin == Builtin::MathImul ? Type::integer(32, false) : Type::intTy();
    break;
  case Pow:
    break;
  }

  return call;
}

const Type *MathChecker::sharedType(std::vector<Expr *> &args, const std::vector<bool> &spreads,
                                    const Type *common) {
  auto spread = [&](size_t i) { return i < spreads.size() && spreads[i]; };
  for (size_t i = 0; i < args.size() && !common; ++i) {
    if (!spread(i) && !TypeRelations::isIntLiteralTree(args[i])) {
      common = args[i]->type;
    }
  }
  if (!common) {
    common = Type::intTy();
    for (size_t i = 0; i < args.size(); ++i) {
      if (!spread(i) && args[i]->type->isFloat()) {
        common = Type::doubleTy();
      }
    }
  }
  for (size_t i = 0; i < args.size(); ++i) {
    if (spread(i) || args[i]->type == common) {
      continue;
    }
    if (TypeRelations(this->state).literalAdapts(args[i], common)) {
      args[i] = TypeRelations(this->state).adaptLiteral(args[i], common);
      continue;
    }
    this->state.getDiagnostics().error(
        args[i]->loc(), "the values of 'Math.min', 'Math.max' and 'Math.clamp' must have one "
                        "type; found '" +
                            common->name().str() + "' and '" + args[i]->type->name().str() + "'" +
                            TypeRelations::conversionHint(args[i], common));
    return nullptr;
  }

  return common;
}

Expr *MathChecker::bitsArg(Expr *arg, unsigned bits, const Type *literal, const std::string &what) {
  if (!arg->type) {
    const Type *hint = literal ? literal : Type::integer(bits, true);
    arg =
        ExprChecker(this->state)
            .checkExpr(arg, TypeRelations::isIntLiteralTree(arg) && !isa<NameExpr>(arg) ? hint
                                                                                        : nullptr);
  }
  const Type *type = arg->type;
  if (type->isError()) {
    return arg;
  }
  if (type->isInteger() && type->bitWidth() == bits) {
    return arg;
  }
  const Type *target = literal ? literal : Type::integer(bits, true);
  if (TypeRelations(this->state).literalAdapts(arg, target)) {
    return TypeRelations(this->state).adaptLiteral(arg, target);
  }
  if (literal && TypeRelations(this->state).literalAdapts(arg, Type::integer(bits, true))) {
    return TypeRelations(this->state).adaptLiteral(arg, Type::integer(bits, true));
  }
  std::string names = bits == 32 ? "int32, uint32 or Bit.Wrap32" : "int, uint64 or Bit.Wrap64";
  std::string message = "'" + what + "' takes a " + std::to_string(bits) + "-bit integer (" +
                        names + "), found '" + type->name().str() + "'";
  if (type->isInteger()) {
    message += TypeRelations::conversionHint(arg, Type::integer(bits, true));
  } else if (type->isFloat()) {
    message += "; drop the fraction first, e.g. 'Math.trunc(x)." +
               TypeRelations::conversionMethod(Type::integer(bits, true)) + "()'";
  }
  this->state.getDiagnostics().error(arg->loc(), message);
  arg->type = Type::error();

  return arg;
}

Expr *MathChecker::checkBitCall(CallExpr *call, MemberExpr *member) {
  call->type = Type::error();
  auto loosely = [&] {
    for (Expr *&arg : call->args) {
      arg = ExprChecker(this->state).checkExpr(arg);
    }

    return call;
  };
  if (auto *inner = dyn_cast<MemberExpr>(member->object)) {
    unsigned bits = inner->name == "Wrap32" ? 32 : inner->name == "Wrap64" ? 64 : 0;
    if (!bits) {
      this->state.getDiagnostics().error(inner->loc(),
                                         "property '" + inner->name + "' does not exist on 'Bit'");
      return loosely();
    }
    if (member->name != "from") {
      this->state.getDiagnostics().error(member->loc(), "'Bit." + inner->name +
                                                            "' has only 'from': 'Bit." +
                                                            inner->name + ".from(x)'");
      return loosely();
    }
    ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 1);
    if (arity.isErr()) {
      this->state.getDiagnostics().error(member->loc(), arity.error().message);
      return call;
    }
    Expr *&arg = call->args[0];
    arg = ExprChecker(this->state).checkExpr(arg);
    if (!arg->type->isInteger()) {
      if (!arg->type->isError()) {
        this->state.getDiagnostics().error(arg->loc(), "'Bit." + inner->name +
                                                           ".from' takes an integer, found '" +
                                                           arg->type->name() + "'");
      }
      return call;
    }
    call->target = BuiltinCall{.builtin = Builtin::BitWrapFrom};
    call->type = Type::wrapping(bits);
    return call;
  }
  struct Function {
    llvm::StringLiteral name;
    Builtin builtin;
    unsigned bits, args;
    bool counts;
  };
  static const Function functions[] = {
      {"rotateLeft32", Builtin::BitRotateLeft, 32, 2, false},
      {"rotateLeft64", Builtin::BitRotateLeft, 64, 2, false},
      {"rotateRight32", Builtin::BitRotateRight, 32, 2, false},
      {"rotateRight64", Builtin::BitRotateRight, 64, 2, false},
      {"leadingZeros32", Builtin::BitLeadingZeros, 32, 1, true},
      {"leadingZeros64", Builtin::BitLeadingZeros, 64, 1, true},
      {"trailingZeros32", Builtin::BitTrailingZeros, 32, 1, true},
      {"trailingZeros64", Builtin::BitTrailingZeros, 64, 1, true},
      {"onesCount32", Builtin::BitOnesCount, 32, 1, true},
      {"onesCount64", Builtin::BitOnesCount, 64, 1, true},
      {"reverseBytes32", Builtin::BitReverseBytes, 32, 1, false},
      {"reverseBytes64", Builtin::BitReverseBytes, 64, 1, false},
      {"mul64", Builtin::BitMul64, 64, 2, false},
      {"add64", Builtin::BitAdd64, 64, 3, false},
      {"sub64", Builtin::BitSub64, 64, 3, false},
  };
  const Function *fn = nullptr;
  for (const Function &f : functions) {
    if (member->name == f.name) {
      fn = &f;
    }
  }
  if (!fn) {
    this->state.getDiagnostics().error(member->loc(),
                                       "property '" + member->name + "' does not exist on 'Bit'");
    return loosely();
  }
  bool rejected = CallChecker(this->state).rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : ArgumentChecker(this->state).checkArity(call, fn->args, fn->args);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
  }
  if (rejected || arity.isErr()) {
    return call;
  }
  std::string what = "Bit." + member->name.str();
  bool wide = fn->builtin == Builtin::BitMul64 || fn->builtin == Builtin::BitAdd64 ||
              fn->builtin == Builtin::BitSub64;
  const Type *u64 = Type::integer(64, true);
  call->target = BuiltinCall{.builtin = fn->builtin};
  if (wide) {
    for (Expr *&arg : call->args) {
      arg = this->bitsArg(arg, 64, nullptr, what);
      if (arg->type->isError()) {
        return call;
      }
      if (arg->type != u64 && !arg->type->isWrapping()) {
        this->state.getDiagnostics().error(arg->loc(), "'" + what + "' takes a 'uint64', found '" +
                                                           arg->type->name() + "'" +
                                                           TypeRelations::conversionHint(arg, u64));
        return call;
      }
      arg = TypeRelations(this->state).convert(arg, u64);
    }
    llvm::StringRef first = fn->builtin == Builtin::BitMul64   ? "hi"
                            : fn->builtin == Builtin::BitAdd64 ? "sum"
                                                               : "diff";
    llvm::StringRef second = fn->builtin == Builtin::BitMul64   ? "lo"
                             : fn->builtin == Builtin::BitAdd64 ? "carry"
                                                                : "borrow";
    call->type = Type::object({{first.str(), u64, false}, {second.str(), u64, false}});
    return call;
  }
  call->args[0] = this->bitsArg(call->args[0], fn->bits, nullptr, what);
  if (call->args[0]->type->isError()) {
    return call;
  }
  if (fn->args == 2) {
    call->args[1] = ExprChecker(this->state).checkExpr(call->args[1]);
    if (!call->args[1]->type->isInteger()) {
      if (!call->args[1]->type->isError()) {
        this->state.getDiagnostics().error(call->args[1]->loc(),
                                           "the count of '" + what + "' is an integer, found '" +
                                               call->args[1]->type->name() + "'");
      }
      return call;
    }
    call->args[1] = TypeRelations(this->state).convert(call->args[1], call->args[0]->type);
  }
  call->type = fn->counts ? Type::intTy() : call->args[0]->type;

  return call;
}

Expr *MathChecker::checkMinMaxSpread(CallExpr *call, const Type *hint) {
  const Type *common = nullptr;
  bool someValue = false;
  bool ok = true;
  for (size_t i = 0; i < call->args.size(); ++i) {
    Expr *&arg = call->args[i];
    const Type *type;
    if (call->spreads(i)) {
      IteratorMethods(this->state).markFused(arg);
      arg = DictChecker(this->state)
                .iterableOf(
                    ExprChecker(this->state).checkExpr(arg, hint ? Type::array(hint) : nullptr));
      if (arg->type->isIterator()) {
        GeneratorChecker::ResultNextNeedsNoValue nothing =
            GeneratorChecker(this->state).checkSendsNothing(arg->type, "a spread");
        if (nothing.isErr()) {
          this->state.getDiagnostics().error(arg->loc(), nothing.error().message);
        } else {
          auto *values = this->state.getAst().make<ArrayLitExpr>(arg->loc());
          values->elements.push_back({arg, true});
          values->type = Type::array(arg->type->yielded());
          FlowChecker(this->state).calledFunction();
          arg = values;
        }
      }
      type = arg->type->isArray() ? arg->type->inner() : arg->type;
      if (!arg->type->isError() && (!arg->type->isArray() || !type->isNumeric())) {
        this->state.getDiagnostics().error(arg->loc(),
                                           "only an array of numbers can be spread here, found '" +
                                               arg->type->name() + "'");
        ok = false;
      }
      someValue |= IndexProofs(this->state).provesNotEmpty(arg);
    } else {
      arg = this->numericArg(arg, hint);
      type = arg->type;
      someValue = true;
    }
    ok &= type->isNumeric();
    if (ok && call->spreads(i)) {
      if (common && common != type) {
        this->state.getDiagnostics().error(
            arg->loc(), "the values of 'Math." +
                            std::string(call->builtin() == Builtin::MathMin ? "min" : "max") +
                            "' must have one type; found '" + common->name().str() + "' and '" +
                            type->name().str() + "'");
        return call;
      }
      common = type;
    }
  }
  if (!ok) {
    return call;
  }
  std::vector<bool> spreads;
  for (size_t i = 0; i < call->args.size(); ++i) {
    spreads.push_back(call->spreads(i));
  }
  common = this->sharedType(call->args, spreads, common);
  if (!common) {
    return call;
  }
  call->type = someValue ? common : Type::optional(common);
  call->mayBeOutOfRange = !someValue;

  return call;
}

}
