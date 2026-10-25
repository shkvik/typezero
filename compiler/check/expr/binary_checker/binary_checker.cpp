#include "compiler/check/expr/binary_checker/binary_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/unary_checker/unary_checker.h"
#include "compiler/check/expr/concat_checker/concat_checker.h"
#include "compiler/check/classes/instance_of_checker/instance_of_checker.h"
#include "compiler/declare/constant_folder/constant_folder.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/expr/truth_checker/truth_checker.h"
#include "compiler/check/types/literal_types/literal_types.h"
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
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

BinaryChecker::BinaryChecker(Sema &sema) : sema_(sema) {}

void BinaryChecker::reportOperandTypes(llvm::SMLoc loc, const char *op, const Type *lhs, const Type *rhs) {
  std::string message = (llvm::Twine("operator '") + op + "' cannot be applied to types '" +
                         lhs->name() + "' and '" + rhs->name() + "'")
                            .str();
  if (lhs->isOptional() || rhs->isOptional() || lhs->isUndefined() || rhs->isUndefined()) {
    message += "; a value may be undefined: check it first or give a default with '??'";
  }
  sema_.diags_.error(loc, message);
}

Expr *BinaryChecker::checkBinary(BinaryExpr *bin, const Type *expected, bool wraps) {
  const char *op = opSpelling(bin->op);
  if (expected) {
    expected = expected->nonOptional();
  }
  switch (bin->op) {
  case BinaryOp::In:
    return sema_.unaryChecker().checkIn(bin);

  case BinaryOp::InstanceOf:
    return sema_.instanceOfChecker().checkInstanceOf(bin);

  case BinaryOp::LogAnd:
  case BinaryOp::LogOr: {
    Sema::Flow whenTrue, whenFalse;
    sema_.truthChecker().checkLogical(bin, expected, whenTrue, whenFalse, /*tested=*/false,
                 bin->op == BinaryOp::LogAnd ? Sema::TruthUse::And : Sema::TruthUse::Or);
    sema_.flow_ = FlowChecker::join(whenTrue, whenFalse);
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
    sema_.decimalChecker().checkComparisonOperands(bin);
    sema_.resultTypes().neverOperands(bin);
    bin->type = Type::boolTy();
    // Two values of one type parameter compare as TypeScript allows; each
    // instance checks its type (objects do not compare).
    if (bin->lhs->type->isParam() && bin->lhs->type == bin->rhs->type) {
      return bin;
    }
    bin->lhs = sema_.genericChecker().apparent(bin->lhs);
    bin->rhs = sema_.genericChecker().apparent(bin->rhs);
    const Type *l = bin->lhs->type, *r = bin->rhs->type;
    if (l->isError() || r->isError()) {
      return bin;
    }
    // Strings compare by UTF-16 code units, as in JavaScript.
    if (l->isString() && r->isString()) {
      return bin;
    }
    if (Expr *dates = sema_.dateChecker().checkDateOperands(bin)) {
      return dates;
    }
    if (l->isDecimal() || r->isDecimal()) {
      return sema_.decimalChecker().checkDecimalComparison(bin);
    }
    if (l->isNumeric() && r->isNumeric()) {
      if (const Type *common = TypeRelations::commonNumericType(l, r)) {
        bin->lhs = sema_.typeRelations().convert(bin->lhs, common);
        bin->rhs = sema_.typeRelations().convert(bin->rhs, common);
        return bin;
      }
    }
    reportOperandTypes(bin->loc(), op, l, r);
    return bin;
  }

  case BinaryOp::Shl:
  case BinaryOp::Shr:
  case BinaryOp::UShr: {
    // Only the low bits of both sides matter: the count is masked. A literal
    // takes no type from the context, as for `&` and `|`: `-1 >>> 0` is the
    // 4294967295 of JavaScript where a long is expected too, not a 64-bit
    // shift of a long -1.
    bin->lhs = sema_.genericChecker().apparent(sema_.exprChecker().checkExpr(bin->lhs, nullptr, /*wraps=*/true));
    bin->rhs = sema_.genericChecker().apparent(sema_.exprChecker().checkExpr(bin->rhs, nullptr, /*wraps=*/true));
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
      // A double becomes a 32-bit integer first, as in JavaScript: ToInt32,
      // whose bits are those of ToUint32, which `>>>` reads them as below.
      bin->lhs = toInt32(bin->lhs);
      l = Type::intTy();
    }
    if (r->isFloat()) {
      bin->rhs = toInt32(bin->rhs);
    }
    // The result has the type of the left side. The shift count is masked to
    // the bit width, so converting it to that type loses nothing.
    bin->rhs = sema_.typeRelations().convert(bin->rhs, l);
    bin->type = l;
    // JavaScript's `x >>> n` is unsigned. Shifted by 1 to 31 bits an int
    // still fits in an int, but `-1 >>> 0` is 4294967295, so with a count
    // that may be 0 the result is a long, unless only its low 32 bits matter
    // (`(h >>> 0) | 0`) or it is a constant that fits.
    if (bin->op == BinaryOp::UShr && l->isInt() && !wraps) {
      std::optional<int64_t> count = sema_.constantFolder().intConstant(bin->rhs);
      if (!count || (*count & 31) == 0) {
        std::optional<int64_t> value = sema_.constantFolder().intConstant(bin->lhs);
        if (!value ||
            static_cast<uint32_t>(*value) > uint32_t(std::numeric_limits<int32_t>::max())) {
          bin->type = Type::longTy();
        }
      }
    }
    sema_.constantFolder().foldBinary(bin, wraps);
    return bin;
  }

  default: { // arithmetic and bitwise
    bool bitwise =
        bin->op == BinaryOp::BitAnd || bin->op == BinaryOp::BitOr || bin->op == BinaryOp::BitXor;
    // The operands of `&`, `|` and `^` take no type from the context: they
    // become 32-bit integers the way JavaScript makes them, and `x | 0` stays
    // ToInt32 wherever it is (`2166136261 | 0` cuts the literal to 32 bits,
    // `Math.floor(x) | 0` wraps around).
    const Type *hint = nullptr;
    if (expected && !bitwise && (expected->isNumeric() || expected->isDecimal())) {
      hint = expected;
    }
    // A decimal power takes an integer exponent, so the exponent takes no
    // decimal from the context.
    bool pow = bin->op == BinaryOp::Pow;
    const Type *rhsHint = pow && hint && hint->isDecimal() ? nullptr : hint;
    // The low bits of a sum, difference, product or power depend only on the
    // low bits of the operands; not so for a quotient.
    bool operandsWrap = bitwise || (wraps && bin->op != BinaryOp::Div && bin->op != BinaryOp::Rem);
    // In the chain `a + b + c` the last `+` is checked first; it reports
    // joining numbers with strings for the whole chain (see reportMixedConcat).
    BinaryExpr *chain = std::exchange(sema_.concatChain_, nullptr);
    bool chainEnd = bin->op == BinaryOp::Add && !chain;
    const BinaryExpr *outerMixed = chainEnd ? std::exchange(sema_.mixedConcat_, nullptr) : nullptr;
    if (auto *left = dyn_cast<BinaryExpr>(bin->lhs);
        bin->op == BinaryOp::Add && left && left->op == BinaryOp::Add) {
      sema_.concatChain_ = chain ? chain : bin;
    }
    // Literals next to a decimal are decimals (see sema_decimal.cpp): a
    // literal left side waits for the right one.
    bool literals = !bitwise && !pow;
    if (literals && DecimalChecker::isLiteralTree(bin->lhs) && !DecimalChecker::isLiteralTree(bin->rhs)) {
      BinaryExpr *pending = std::exchange(sema_.concatChain_, nullptr);
      bin->rhs = sema_.genericChecker().apparent(sema_.exprChecker().checkExpr(bin->rhs, rhsHint, operandsWrap));
      sema_.concatChain_ = pending;
      bin->lhs = sema_.genericChecker().apparent(sema_.exprChecker().checkExpr(bin->lhs, DecimalChecker::literalHint(bin->rhs, hint), operandsWrap));
      sema_.concatChain_ = nullptr;
    } else {
      bin->lhs = sema_.genericChecker().apparent(sema_.exprChecker().checkExpr(bin->lhs, hint, operandsWrap));
      sema_.concatChain_ = nullptr;
      if (literals && DecimalChecker::isLiteralTree(bin->rhs)) {
        rhsHint = DecimalChecker::literalHint(bin->lhs, rhsHint);
      }
      bin->rhs = sema_.genericChecker().apparent(sema_.exprChecker().checkExpr(bin->rhs, rhsHint, operandsWrap));
    }
    sema_.resultTypes().neverOperands(bin);
    const Type *l = bin->lhs->type, *r = bin->rhs->type;
    if (bin->op == BinaryOp::Add) {
      // JavaScript joins a number or a boolean with a string as text: `10 + "1"`
      // is "101", which is rarely meant where it is not written as a template.
      auto joinsAsText = [](const Type *type) {
        const Type *base = type->nonOptional();

        return base->isNumeric() || base->isDecimal() || base->isBool();
      };
      if (!sema_.mixedConcat_ &&
          ((l->isString() && joinsAsText(r)) || (r->isString() && joinsAsText(l)))) {
        sema_.mixedConcat_ = bin;
      }
      if (chainEnd) {
        if (sema_.mixedConcat_) {
          sema_.concatChecker().reportMixedConcat(bin, sema_.mixedConcat_);
        }
        sema_.mixedConcat_ = outerMixed;
      }
    }
    bin->type = Type::error();
    if (l->isError() || r->isError()) {
      return bin;
    }
    if (bin->op == BinaryOp::Add && (l->isString() || r->isString())) {
      // As in JavaScript, `+` concatenates when either side is a string.
      bin->lhs = sema_.concatChecker().toStringOperand(bin->lhs);
      bin->rhs = sema_.concatChecker().toStringOperand(bin->rhs);
      bin->type = Type::string();
      return bin;
    }
    if (Expr *dates = sema_.dateChecker().checkDateOperands(bin)) {
      return dates;
    }
    if (!bitwise && (l->isDecimal() || r->isDecimal())) {
      return sema_.decimalChecker().checkDecimalArithmetic(bin);
    }
    if (bitwise && l->isNumeric() && r->isNumeric() && (l->isFloat() || r->isFloat() || l != r)) {
      // As in JavaScript, both sides become 32-bit integers first, so `x | 0`
      // drops the fraction of a double exactly the way JavaScript does. A
      // long next to an int keeps its low 32 bits the same way: `x | 0` is
      // JavaScript's ToInt32 for a long too. Only two longs work on 64 bits.
      bin->lhs = toInt32(bin->lhs);
      bin->rhs = toInt32(bin->rhs);
      bin->type = Type::intTy();
      sema_.constantFolder().foldBinary(bin, wraps);
      return bin;
    }
    bool operandsOk = bitwise ? l->isInteger() && r->isInteger() : l->isNumeric() && r->isNumeric();
    const Type *common = operandsOk ? TypeRelations::commonNumericType(l, r) : nullptr;
    if (!common) {
      reportOperandTypes(bin->loc(), op, l, r);
      return bin;
    }
    bin->lhs = sema_.typeRelations().convert(bin->lhs, common);
    bin->rhs = sema_.typeRelations().convert(bin->rhs, common);
    bin->type = common;
    if (!common->isInteger()) {
      return bin;
    }
    // An integer to a negative power is a fraction (as with `/`, integers
    // stay integers); a negative constant exponent is surely a mistake.
    if (bin->op == BinaryOp::Pow) {
      if (std::optional<int64_t> exponent = sema_.constantFolder().intConstant(bin->rhs); exponent && *exponent < 0) {
        sema_.diags_.error(bin->rhs->loc(), "'" + common->name() +
                                          "' raised to a negative power is a fraction, which an "
                                          "integer cannot hold; use a double, e.g. '2.0 ** -1'");
        return bin;
      }
    }
    sema_.constantFolder().checkDivisor(bin->op, bin->rhs);
    sema_.constantFolder().foldBinary(bin, wraps);
    return bin;
  }
  }
}

// `==` and `!=` (the only equality operators; there is no null to convert). Any
// value can be compared with undefined; `T | undefined` compares with `T`.
Expr *BinaryChecker::checkEquality(BinaryExpr *bin) {
  if (Expr *null = sema_.jsonChecker().checkNullEquality(bin)) {
    return null;
  }
  sema_.decimalChecker().checkComparisonOperands(bin);
  const Type *l = bin->lhs->type, *r = bin->rhs->type;
  bin->type = Type::boolTy();
  // typeof never gives "bigint": there is none.
  for (auto [side, other] : {std::pair{bin->lhs, bin->rhs}, std::pair{bin->rhs, bin->lhs}}) {
    auto *typeOf = dyn_cast<UnaryExpr>(side);
    auto *text = dyn_cast<StringLitExpr>(other);
    if (typeOf && typeOf->op == UnaryOp::TypeOf && text && text->value == u"bigint") {
      sema_.decimalChecker().reportBigInt(text->loc(), "'typeof x == \"bigint\"'");
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
    return sema_.jsonChecker().checkJsonEquality(bin);
  }
  // In JavaScript two Results are two objects, never equal; their values
  // cannot be compared for them.
  if (l->nonOptional()->isResult() || r->nonOptional()->isResult()) {
    sema_.diags_.error(bin->loc(), llvm::Twine("Results cannot be compared with '") +
                                 opSpelling(bin->op) +
                                 "': in JavaScript 'ok(1) == ok(1)' is false, as they are two "
                                 "objects; compare what they hold, with 'isOk()', 'match' or "
                                 "'value'");
    return bin;
  }
  // TypeScript compares a value of a type parameter with anything; each
  // instance checks the types it has.
  if (l->nonOptional()->isParam() || r->nonOptional()->isParam()) {
    return bin;
  }

  const Type *common = TypeRelations::comparisonType(l, r);
  if (!common) {
    if (l->nonOptional()->isDecimal() || r->nonOptional()->isDecimal()) {
      sema_.decimalChecker().reportDecimalMix(bin);
    } else {
      reportOperandTypes(bin->loc(), opSpelling(bin->op), l, r);
    }
    return bin;
  }
  if (common->nonOptional()->isFunction()) {
    sema_.file_->comparesFunctions = true;
  }
  bin->lhs = sema_.typeRelations().convert(bin->lhs, common);
  bin->rhs = sema_.typeRelations().convert(bin->rhs, common);

  return bin;
}

// `a ?? b`: b when a is undefined. When a cannot be undefined (for example an
// index Sema proved in range), b is checked but never evaluated.
Expr *BinaryChecker::checkNullish(BinaryExpr *bin, const Type *expected) {
  bin->lhs = sema_.exprChecker().checkExpr(bin->lhs, expected);
  // `s.match(re) ?? []`: the match is the array of its captures there, as
  // it is an array in JavaScript.
  if (bin->lhs->type->nonOptional()->isMatch() && isa<ArrayLitExpr>(bin->rhs)) {
    bin->lhs = sema_.matchChecker().matchItems(bin->lhs);
  }
  const Type *l = bin->lhs->type;
  Sema::Flow afterLhs = sema_.flow_;
  const Type *hint = l->isUndefined() || l->isError() ? expected : l->nonOptional();
  bin->rhs = sema_.exprChecker().checkExpr(bin->rhs, hint);
  sema_.flow_ = FlowChecker::join(afterLhs, sema_.flow_); // the right side may not run
  const Type *r = bin->rhs->type;
  bin->type = Type::error();
  if (l->isError() || r->isError()) {
    if (sema_.trial_ && l->isError() != r->isError()) { // as in checkConditional
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
  const Type *base =
      r->isUndefined() ? l->nonOptional() : LiteralTypes::unionUnify(l->nonOptional(), r->nonOptional());
  if (!base || base->isError()) {
    reportOperandTypes(bin->loc(), "??", l, r);
    return bin;
  }
  const Type *result = r->isOptional() || r->isUndefined() ? Type::optional(base) : base;
  bin->lhs = sema_.typeRelations().convert(bin->lhs, l->isOptional() ? Type::optional(base) : result);
  bin->rhs = sema_.typeRelations().convert(bin->rhs, result);
  bin->type = result;

  return bin;
}

Expr *BinaryChecker::toInt32(Expr *expr) {
  return expr->type->isInt() || expr->type->isError() ? expr : sema_.typeRelations().convert(expr, Type::intTy());
}

} // namespace tz
