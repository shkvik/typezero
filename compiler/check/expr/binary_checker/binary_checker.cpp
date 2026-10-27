#include "compiler/check/expr/binary_checker/binary_checker.h"
#include "compiler/check/classes/instance_of_checker/instance_of_checker.h"
#include "compiler/check/expr/concat_checker/concat_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/truth_checker/truth_checker.h"
#include "compiler/check/expr/unary_checker/unary_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/types/literal_types/literal_types.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/constant_folder/constant_folder.h"
#include "compiler/library/date/date_checker/date_checker.h"
#include "compiler/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/library/json/json_checker/json_checker.h"
#include "compiler/library/regex/match_checker/match_checker.h"
#include "compiler/library/result/result_types/result_types.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

bool onlyIntLiterals(const Expr *expr) {
  if (isa<IntLitExpr>(expr)) {
    return true;
  }
  if (auto *unary = dyn_cast<UnaryExpr>(expr)) {
    return (unary->op == UnaryOp::Neg || unary->op == UnaryOp::Plus) &&
           onlyIntLiterals(unary->operand);
  }
  if (auto *bin = dyn_cast<BinaryExpr>(expr)) {
    return bin->op != BinaryOp::Div && DecimalChecker::isLiteralTree(bin) &&
           onlyIntLiterals(bin->lhs) && onlyIntLiterals(bin->rhs);
  }

  return false;
}

const Type *integerHint(const Expr *literal, const Expr *other, const Type *hint, bool bitwise) {
  const Type *type = other->type ? other->type->nonOptional() : nullptr;
  if (type && (type->isInteger() || (type->isFloat() && !bitwise)) && onlyIntLiterals(literal) &&
      !TypeRelations::isIntLiteralTree(other)) {
    return type;
  }

  return hint;
}

}

BinaryChecker::BinaryChecker(Checker &checker) : checker_(checker) {}

void BinaryChecker::reportOperandTypes(llvm::SMLoc loc, const char *op, const Type *lhs,
                                       const Type *rhs) {
  std::string message = (llvm::Twine("operator '") + op + "' cannot be applied to types '" +
                         lhs->name() + "' and '" + rhs->name() + "'")
                            .str();
  if (lhs->isOptional() || rhs->isOptional() || lhs->isUndefined() || rhs->isUndefined()) {
    message += "; a value may be undefined: check it first or give a default with '??'";
  }
  checker_.diags_.error(loc, message);
}

Expr *BinaryChecker::checkBinary(BinaryExpr *bin, const Type *expected, bool wraps) {
  const char *op = opSpelling(bin->op);
  if (expected) {
    expected = expected->nonOptional();
  }
  switch (bin->op) {
  case BinaryOp::In:
    return checker_.unaryChecker().checkIn(bin);

  case BinaryOp::InstanceOf:
    return checker_.instanceOfChecker().checkInstanceOf(bin);

  case BinaryOp::LogAnd:
  case BinaryOp::LogOr: {
    Checker::Flow whenTrue, whenFalse;
    checker_.truthChecker().checkLogical(bin, expected, whenTrue, whenFalse, false,
                                         bin->op == BinaryOp::LogAnd ? Checker::TruthUse::And
                                                                     : Checker::TruthUse::Or);
    checker_.flow_ = FlowChecker::join(whenTrue, whenFalse);
    return bin;
  }

  case BinaryOp::Nullish:
    return checkNullish(bin, expected);

  case BinaryOp::Eq:
  case BinaryOp::Ne:
    return checkEquality(bin);

  case BinaryOp::Lt:
  case BinaryOp::Le:
  case BinaryOp::Gt:
  case BinaryOp::Ge: {
    checker_.decimalChecker().checkComparisonOperands(bin);
    checker_.resultTypes().neverOperands(bin);
    bin->type = Type::boolTy();
    if (bin->lhs->type->isParam() && bin->lhs->type == bin->rhs->type) {
      return bin;
    }
    bin->lhs = checker_.genericChecker().apparent(bin->lhs);
    bin->rhs = checker_.genericChecker().apparent(bin->rhs);
    const Type *l = bin->lhs->type, *r = bin->rhs->type;
    if (l->isError() || r->isError()) {
      return bin;
    }
    if (l->isString() && r->isString()) {
      return bin;
    }
    if (Expr *dates = checker_.dateChecker().checkDateOperands(bin)) {
      return dates;
    }
    if (l->isDecimal() || r->isDecimal()) {
      return checker_.decimalChecker().checkDecimalComparison(bin);
    }
    if (l->isNumeric() && r->isNumeric()) {
      adaptLiterals(bin);
      l = bin->lhs->type;
      r = bin->rhs->type;
      if (l == r || (l->isInteger() && r->isInteger())) {
        return bin;
      }
      reportMixedNumbers(bin);
      return bin;
    }
    reportOperandTypes(bin->loc(), op, l, r);
    return bin;
  }

  case BinaryOp::Shl:
  case BinaryOp::Shr:
  case BinaryOp::UShr: {
    const Type *hint =
        expected && expected->nonOptional()->isInteger() ? expected->nonOptional() : nullptr;
    bin->lhs = checker_.genericChecker().apparent(
        checker_.exprChecker().checkExpr(bin->lhs, onlyIntLiterals(bin->lhs) ? hint : nullptr));
    bin->rhs = checker_.genericChecker().apparent(checker_.exprChecker().checkExpr(bin->rhs));
    return checkBits(bin);
  }

  default: {
    bool bitwise =
        bin->op == BinaryOp::BitAnd || bin->op == BinaryOp::BitOr || bin->op == BinaryOp::BitXor;
    const Type *hint = nullptr;
    if (expected && !bitwise && (expected->isNumeric() || expected->isDecimal())) {
      hint = expected;
    }
    bool pow = bin->op == BinaryOp::Pow;
    const Type *rhsHint = pow && hint && hint->isDecimal() ? nullptr : hint;
    bool operandsWrap = false;
    BinaryExpr *chain = std::exchange(checker_.concatChain_, nullptr);
    bool chainEnd = bin->op == BinaryOp::Add && !chain;
    const BinaryExpr *outerMixed =
        chainEnd ? std::exchange(checker_.mixedConcat_, nullptr) : nullptr;
    if (auto *left = dyn_cast<BinaryExpr>(bin->lhs);
        bin->op == BinaryOp::Add && left && left->op == BinaryOp::Add) {
      checker_.concatChain_ = chain ? chain : bin;
    }
    bool literals = !bitwise && !pow;
    if (DecimalChecker::isLiteralTree(bin->lhs) && !DecimalChecker::isLiteralTree(bin->rhs) &&
        (literals || onlyIntLiterals(bin->lhs))) {
      BinaryExpr *pending = std::exchange(checker_.concatChain_, nullptr);
      bin->rhs = checker_.genericChecker().apparent(
          checker_.exprChecker().checkExpr(bin->rhs, rhsHint, operandsWrap));
      checker_.concatChain_ = pending;
      const Type *lhsHint =
          integerHint(bin->lhs, bin->rhs,
                      literals ? DecimalChecker::literalHint(bin->rhs, hint) : hint, bitwise);
      bin->lhs = checker_.genericChecker().apparent(
          checker_.exprChecker().checkExpr(bin->lhs, lhsHint, operandsWrap));
      checker_.concatChain_ = nullptr;
    } else {
      bin->lhs = checker_.genericChecker().apparent(
          checker_.exprChecker().checkExpr(bin->lhs, hint, operandsWrap));
      checker_.concatChain_ = nullptr;
      if (literals && DecimalChecker::isLiteralTree(bin->rhs)) {
        rhsHint = DecimalChecker::literalHint(bin->lhs, rhsHint);
      }
      rhsHint = integerHint(bin->rhs, bin->lhs, rhsHint, bitwise);
      bin->rhs = checker_.genericChecker().apparent(
          checker_.exprChecker().checkExpr(bin->rhs, rhsHint, operandsWrap));
    }
    checker_.resultTypes().neverOperands(bin);
    adaptLiterals(bin);
    const Type *l = bin->lhs->type, *r = bin->rhs->type;
    if (bin->op == BinaryOp::Add) {
      auto joinsAsText = [](const Type *type) {
        const Type *base = type->nonOptional();

        return base->isNumeric() || base->isDecimal() || base->isBool();
      };
      if (!checker_.mixedConcat_ &&
          ((l->isString() && joinsAsText(r)) || (r->isString() && joinsAsText(l)))) {
        checker_.mixedConcat_ = bin;
      }
      if (chainEnd) {
        if (checker_.mixedConcat_) {
          checker_.concatChecker().reportMixedConcat(bin, checker_.mixedConcat_);
        }
        checker_.mixedConcat_ = outerMixed;
      }
    }
    bin->type = Type::error();
    if (l->isError() || r->isError()) {
      return bin;
    }
    if (bin->op == BinaryOp::Add && (l->isString() || r->isString())) {
      bin->lhs = checker_.concatChecker().toStringOperand(bin->lhs);
      bin->rhs = checker_.concatChecker().toStringOperand(bin->rhs);
      bin->type = Type::string();
      return bin;
    }
    if (Expr *dates = checker_.dateChecker().checkDateOperands(bin)) {
      return dates;
    }
    if (bitwise) {
      return checkBits(bin);
    }
    if (l->isDecimal() || r->isDecimal()) {
      return checker_.decimalChecker().checkDecimalArithmetic(bin);
    }
    if ((l->isWrapping() || r->isWrapping()) && l->isInteger() && r->isInteger()) {
      return checkWrapping(bin);
    }
    if (bin->op == BinaryOp::Pow && l->isFloat() && r->isInteger()) {
      bin->rhs = checker_.typeRelations().convert(bin->rhs, l);
      r = l;
    }
    bool operandsOk = bitwise ? l->isInteger() && r->isInteger() : l->isNumeric() && r->isNumeric();
    const Type *common = operandsOk ? TypeRelations::commonNumericType(l, r) : nullptr;
    if (!common && operandsOk) {
      reportMixedNumbers(bin);
      return bin;
    }
    if (!common) {
      reportOperandTypes(bin->loc(), op, l, r);
      return bin;
    }
    bin->lhs = checker_.typeRelations().convert(bin->lhs, common);
    bin->rhs = checker_.typeRelations().convert(bin->rhs, common);
    bin->type = common;
    if (!common->isInteger()) {
      return bin;
    }
    if (bin->op == BinaryOp::Pow) {
      if (std::optional<int64_t> exponent = checker_.constantFolder().intConstant(bin->rhs);
          exponent && *exponent < 0) {
        checker_.diags_.error(bin->rhs->loc(),
                              "'" + common->name() +
                                  "' raised to a negative power is a fraction, which an "
                                  "integer cannot hold; use a double, e.g. '2.0 ** -1'");
        return bin;
      }
    }
    checker_.constantFolder().checkDivisor(bin->op, bin->rhs);
    checker_.constantFolder().foldBinary(bin, wraps);
    return bin;
  }
  }
}

Expr *BinaryChecker::checkWrapping(BinaryExpr *bin) {
  const Type *l = bin->lhs->type, *r = bin->rhs->type;
  if (l->isWrapping() && r->isWrapping() && l != r) {
    reportMixedNumbers(bin);
    return bin;
  }
  const Type *wrap = l->isWrapping() ? l : r;
  bin->lhs = checker_.typeRelations().convert(bin->lhs, wrap);
  bin->rhs = checker_.typeRelations().convert(bin->rhs, wrap);
  bin->type = wrap;
  checker_.constantFolder().checkDivisor(bin->op, bin->rhs);

  return bin;
}

Expr *BinaryChecker::checkBits(BinaryExpr *bin) {
  const char *op = opSpelling(bin->op);
  bool shift = bin->op == BinaryOp::Shl || bin->op == BinaryOp::Shr || bin->op == BinaryOp::UShr;
  adaptLiterals(bin);
  const Type *l = bin->lhs->type, *r = bin->rhs->type;
  bin->type = Type::error();
  if (l->isError() || r->isError()) {
    return bin;
  }
  auto zero = [](const Expr *e) {
    auto *lit = dyn_cast<IntLitExpr>(e);

    return lit && lit->value == 0;
  };
  auto [lhsText, rhsText] = DecimalChecker::sidesOf(bin);
  std::string text = lhsText.empty() ? "x" : TypeRelations::operandText(lhsText);
  if (l->isNumeric() && !l->isWrapping() && zero(bin->rhs) &&
      (bin->op == BinaryOp::BitOr || bin->op == BinaryOp::UShr)) {
    std::string message =
        "'" + std::string(lhsText.empty() ? "x" : lhsText.str()) + " " + op +
        " 0' changes nothing in tz: integers are exact, and nothing wraps around here";
    auto *rounded = dyn_cast<CallExpr>(bin->lhs);
    if (l->isFloat() && rounded && isRoundingBuiltin(rounded->builtin)) {
      message += "; to make it an int, write '" + std::string(lhsText.str()) + ".toInt()'";
    } else if (l->isFloat()) {
      message +=
          "; to drop the fraction, write 'Math.trunc(" + std::string(lhsText.str()) + ").toInt()'";
    } else {
      message += "; to make it an int32, write '" + text + ".toInt32()'" +
                 (bin->op == BinaryOp::UShr ? ", a uint32 '" + text + ".toUint32()'" : "") +
                 ", and to count modulo 2^32, use 'Bit.Wrap32'";
    }
    checker_.diags_.error(bin->loc(), message);
    return bin;
  }
  if (l->isFloat() || r->isFloat()) {
    std::string side = l->isFloat()      ? text
                       : rhsText.empty() ? "x"
                                         : TypeRelations::operandText(rhsText);
    checker_.diags_.error(bin->loc(),
                          llvm::Twine("operator '") + op +
                              "' works on the bits of integers, and this is a double; drop the "
                              "fraction first, e.g. 'Math.trunc(" +
                              side + ").toInt()'");
    return bin;
  }
  if (!l->isInteger() || !r->isInteger()) {
    reportOperandTypes(bin->loc(), op, l, r);
    return bin;
  }
  if (shift) {
    if (bin->op == BinaryOp::UShr && !l->isUnsigned()) {
      std::string count = rhsText.empty() ? "n" : rhsText.str();
      checker_.diags_.error(
          bin->loc(), "'>>>' shifts in zeros, as a shift of an unsigned type does; '" + l->name() +
                          "' is signed: use '>>', or convert it, e.g. '" + text + "." +
                          TypeRelations::conversionMethod(Type::integer(l->bitWidth(), true)) +
                          "() >>> " + count + "'");
      return bin;
    }
    bin->rhs = checker_.typeRelations().convert(bin->rhs, l);
    bin->type = l;
    checker_.constantFolder().foldBinary(bin, false);
    return bin;
  }
  if (l->isWrapping() || r->isWrapping()) {
    return checkWrapping(bin);
  }
  if (l != r) {
    reportMixedNumbers(bin);
    return bin;
  }
  bin->type = l;
  checker_.constantFolder().foldBinary(bin, false);

  return bin;
}

void BinaryChecker::adaptLiterals(BinaryExpr *bin) {
  const Type *l = bin->lhs->type, *r = bin->rhs->type;
  if (!l || !r || l == r || l->isError() || r->isError()) {
    return;
  }
  auto number = [](const Type *t) { return t->isNumeric() || t->isDecimal(); };
  bool bitwise = bin->op == BinaryOp::BitAnd || bin->op == BinaryOp::BitOr ||
                 bin->op == BinaryOp::BitXor || bin->op == BinaryOp::Shl ||
                 bin->op == BinaryOp::Shr || bin->op == BinaryOp::UShr;
  if (!number(l) || !number(r) || (bitwise && (!l->isInteger() || !r->isInteger()))) {
    return;
  }
  if (checker_.typeRelations().literalAdapts(bin->lhs, r) &&
      !(bin->op == BinaryOp::Pow && r->isDecimal())) {
    bin->lhs = checker_.typeRelations().adaptLiteral(bin->lhs, r);
  } else if (checker_.typeRelations().literalAdapts(bin->rhs, l) &&
             !(bin->op == BinaryOp::Pow && (l->isDecimal() || l->isFloat()))) {
    bin->rhs = checker_.typeRelations().adaptLiteral(bin->rhs, l);
  }
}

void BinaryChecker::reportMixedNumbers(const BinaryExpr *bin) {
  const Type *l = bin->lhs->type, *r = bin->rhs->type;
  const char *op = opSpelling(bin->op);
  std::string message = (llvm::Twine("operator '") + op + "' cannot be applied to types '" +
                         l->name() + "' and '" + r->name() + "'")
                            .str();
  const Type *target = nullptr;
  bool convertLeft = false;
  if (l->isFloat() || r->isFloat()) {
    target = Type::doubleTy();
    convertLeft = !l->isFloat();
  } else if (l->holds(r)) {
    target = l;
  } else if (r->holds(l)) {
    target = r;
    convertLeft = true;
  } else {
    target = Type::intTy();
    convertLeft = !l->isInt();
  }
  message += ": a number changes its type only explicitly";
  auto [lhs, rhs] = DecimalChecker::sidesOf(bin);
  std::string method = TypeRelations::conversionMethod(target);
  if (!lhs.empty() && !rhs.empty()) {
    std::string left =
        convertLeft ? TypeRelations::operandText(lhs) + "." + method + "()" : lhs.str();
    std::string right =
        convertLeft ? rhs.str() : TypeRelations::operandText(rhs) + "." + method + "()";
    message += ", e.g. '" + left + " " + op + " " + right + "'";
  } else {
    message += " with '." + method + "()'";
  }
  checker_.diags_.error(bin->loc(), message);
}

Expr *BinaryChecker::checkEquality(BinaryExpr *bin) {
  if (Expr *null = checker_.jsonChecker().checkNullEquality(bin)) {
    return null;
  }
  checker_.decimalChecker().checkComparisonOperands(bin);
  const Type *l = bin->lhs->type, *r = bin->rhs->type;
  bin->type = Type::boolTy();
  for (auto [side, other] : {std::pair{bin->lhs, bin->rhs}, std::pair{bin->rhs, bin->lhs}}) {
    auto *typeOf = dyn_cast<UnaryExpr>(side);
    auto *text = dyn_cast<StringLitExpr>(other);
    if (typeOf && typeOf->op == UnaryOp::TypeOf && text && text->value == u"bigint") {
      checker_.decimalChecker().reportBigInt(text->loc(), "'typeof x == \"bigint\"'");
    }
  }
  if (l->isError() || r->isError()) {
    return bin;
  }
  if (l->isVoid() || r->isVoid()) {
    reportOperandTypes(bin->loc(), opSpelling(bin->op), l, r);
    return bin;
  }
  if (l->isUndefined() || r->isUndefined()) {
    return bin;
  }
  if (l->nonOptional()->isJson() || r->nonOptional()->isJson()) {
    return checker_.jsonChecker().checkJsonEquality(bin);
  }
  if (l->nonOptional()->isResult() || r->nonOptional()->isResult()) {
    checker_.diags_.error(bin->loc(),
                          llvm::Twine("Results cannot be compared with '") + opSpelling(bin->op) +
                              "': in JavaScript 'ok(1) == ok(1)' is false, as they are two "
                              "objects; compare what they hold, with 'isOk()', 'match' or "
                              "'value'");
    return bin;
  }
  if (l->nonOptional()->isParam() || r->nonOptional()->isParam()) {
    return bin;
  }

  adaptLiterals(bin);
  l = bin->lhs->type;
  r = bin->rhs->type;
  if (l->isInteger() && r->isInteger()) {
    return bin;
  }
  const Type *common = TypeRelations::comparisonType(l, r);
  if (!common) {
    if (l->nonOptional()->isDecimal() || r->nonOptional()->isDecimal()) {
      checker_.decimalChecker().reportDecimalMix(bin);
    } else if (l->nonOptional()->isNumeric() && r->nonOptional()->isNumeric() && !l->isOptional() &&
               !r->isOptional()) {
      reportMixedNumbers(bin);
    } else {
      reportOperandTypes(bin->loc(), opSpelling(bin->op), l, r);
    }
    return bin;
  }
  if (common->nonOptional()->isFunction()) {
    checker_.file_->comparesFunctions = true;
  }
  bin->lhs = checker_.typeRelations().convert(bin->lhs, common);
  bin->rhs = checker_.typeRelations().convert(bin->rhs, common);

  return bin;
}

Expr *BinaryChecker::checkNullish(BinaryExpr *bin, const Type *expected) {
  bin->lhs = checker_.exprChecker().checkExpr(bin->lhs, expected);
  if (bin->lhs->type->nonOptional()->isMatch() && isa<ArrayLitExpr>(bin->rhs)) {
    bin->lhs = checker_.matchChecker().matchItems(bin->lhs);
  }
  const Type *l = bin->lhs->type;
  Checker::Flow afterLhs = checker_.flow_;
  const Type *hint = l->isUndefined() || l->isError() ? expected : l->nonOptional();
  bin->rhs = checker_.exprChecker().checkExpr(bin->rhs, hint);
  checker_.flow_ = FlowChecker::join(afterLhs, checker_.flow_);
  const Type *r = bin->rhs->type;
  bin->type = Type::error();
  if (l->isError() || r->isError()) {
    if (checker_.trial_ && l->isError() != r->isError()) {
      bin->type = l->isError() ? r : l->nonOptional();
    }
    return bin;
  }
  if (l->isVoid() || r->isVoid()) {
    reportOperandTypes(bin->loc(), "??", l, r);
    return bin;
  }
  if (l->isUndefined()) {
    bin->type = r;
    return bin;
  }
  const Type *base = r->isUndefined()
                         ? l->nonOptional()
                         : LiteralTypes::unionUnify(l->nonOptional(), r->nonOptional());
  if (!base || base->isError()) {
    reportOperandTypes(bin->loc(), "??", l, r);
    return bin;
  }
  const Type *result = r->isOptional() || r->isUndefined() ? Type::optional(base) : base;
  bin->lhs =
      checker_.typeRelations().convert(bin->lhs, l->isOptional() ? Type::optional(base) : result);
  bin->rhs = checker_.typeRelations().convert(bin->rhs, result);
  bin->type = result;

  return bin;
}

Expr *BinaryChecker::toInt32(Expr *expr) {
  return expr->type->isInt() || expr->type->isError()
             ? expr
             : checker_.typeRelations().convert(expr, Type::intTy());
}

}
