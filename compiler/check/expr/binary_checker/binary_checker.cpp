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
      if (const Type *common = TypeRelations::commonNumericType(l, r)) {
        bin->lhs = checker_.typeRelations().convert(bin->lhs, common);
        bin->rhs = checker_.typeRelations().convert(bin->rhs, common);
        return bin;
      }
    }
    reportOperandTypes(bin->loc(), op, l, r);
    return bin;
  }

  case BinaryOp::Shl:
  case BinaryOp::Shr:
  case BinaryOp::UShr: {
    bin->lhs = checker_.genericChecker().apparent(
        checker_.exprChecker().checkExpr(bin->lhs, nullptr, true));
    bin->rhs = checker_.genericChecker().apparent(
        checker_.exprChecker().checkExpr(bin->rhs, nullptr, true));
    const Type *l = bin->lhs->type, *r = bin->rhs->type;
    bin->type = Type::error();
    if (l->isError() || r->isError()) {
      return bin;
    }
    if (!l->isNumeric() || !r->isNumeric()) {
      reportOperandTypes(bin->loc(), op, l, r);
      return bin;
    }
    if (l->isFloat()) {
      bin->lhs = toInt32(bin->lhs);
      l = Type::intTy();
    }
    if (r->isFloat()) {
      bin->rhs = toInt32(bin->rhs);
    }
    bin->rhs = checker_.typeRelations().convert(bin->rhs, l);
    bin->type = l;
    if (bin->op == BinaryOp::UShr && l->isInt() && !wraps) {
      std::optional<int64_t> count = checker_.constantFolder().intConstant(bin->rhs);
      if (!count || (*count & 31) == 0) {
        std::optional<int64_t> value = checker_.constantFolder().intConstant(bin->lhs);
        if (!value ||
            static_cast<uint32_t>(*value) > uint32_t(std::numeric_limits<int32_t>::max())) {
          bin->type = Type::longTy();
        }
      }
    }
    checker_.constantFolder().foldBinary(bin, wraps);
    return bin;
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
    bool operandsWrap = bitwise || (wraps && bin->op != BinaryOp::Div && bin->op != BinaryOp::Rem);
    BinaryExpr *chain = std::exchange(checker_.concatChain_, nullptr);
    bool chainEnd = bin->op == BinaryOp::Add && !chain;
    const BinaryExpr *outerMixed =
        chainEnd ? std::exchange(checker_.mixedConcat_, nullptr) : nullptr;
    if (auto *left = dyn_cast<BinaryExpr>(bin->lhs);
        bin->op == BinaryOp::Add && left && left->op == BinaryOp::Add) {
      checker_.concatChain_ = chain ? chain : bin;
    }
    bool literals = !bitwise && !pow;
    if (literals && DecimalChecker::isLiteralTree(bin->lhs) &&
        !DecimalChecker::isLiteralTree(bin->rhs)) {
      BinaryExpr *pending = std::exchange(checker_.concatChain_, nullptr);
      bin->rhs = checker_.genericChecker().apparent(
          checker_.exprChecker().checkExpr(bin->rhs, rhsHint, operandsWrap));
      checker_.concatChain_ = pending;
      bin->lhs = checker_.genericChecker().apparent(checker_.exprChecker().checkExpr(
          bin->lhs, DecimalChecker::literalHint(bin->rhs, hint), operandsWrap));
      checker_.concatChain_ = nullptr;
    } else {
      bin->lhs = checker_.genericChecker().apparent(
          checker_.exprChecker().checkExpr(bin->lhs, hint, operandsWrap));
      checker_.concatChain_ = nullptr;
      if (literals && DecimalChecker::isLiteralTree(bin->rhs)) {
        rhsHint = DecimalChecker::literalHint(bin->lhs, rhsHint);
      }
      bin->rhs = checker_.genericChecker().apparent(
          checker_.exprChecker().checkExpr(bin->rhs, rhsHint, operandsWrap));
    }
    checker_.resultTypes().neverOperands(bin);
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
    if (!bitwise && (l->isDecimal() || r->isDecimal())) {
      return checker_.decimalChecker().checkDecimalArithmetic(bin);
    }
    if (bitwise && l->isNumeric() && r->isNumeric() && (l->isFloat() || r->isFloat() || l != r)) {
      bin->lhs = toInt32(bin->lhs);
      bin->rhs = toInt32(bin->rhs);
      bin->type = Type::intTy();
      checker_.constantFolder().foldBinary(bin, wraps);
      return bin;
    }
    bool operandsOk = bitwise ? l->isInteger() && r->isInteger() : l->isNumeric() && r->isNumeric();
    const Type *common = operandsOk ? TypeRelations::commonNumericType(l, r) : nullptr;
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

  const Type *common = TypeRelations::comparisonType(l, r);
  if (!common) {
    if (l->nonOptional()->isDecimal() || r->nonOptional()->isDecimal()) {
      checker_.decimalChecker().reportDecimalMix(bin);
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
