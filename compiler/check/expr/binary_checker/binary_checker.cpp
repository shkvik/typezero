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
#include "compiler/program/types/type_spelling/type_spelling.h"
#include "compiler/load/to_utf8/to_utf8.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

bool BinaryChecker::onlyIntLiterals(const Expr *expr) {
  if (isa<IntLitExpr>(expr)) {
    return true;
  }
  if (auto *unary = dyn_cast<UnaryExpr>(expr)) {
    return (unary->op == UnaryOp::Neg || unary->op == UnaryOp::Plus) &&
           BinaryChecker::onlyIntLiterals(unary->operand);
  }
  if (auto *bin = dyn_cast<BinaryExpr>(expr)) {
    return bin->op != BinaryOp::Div && DecimalChecker::isLiteralTree(bin) &&
           BinaryChecker::onlyIntLiterals(bin->lhs) && BinaryChecker::onlyIntLiterals(bin->rhs);
  }

  return false;
}

const Type *BinaryChecker::integerHint(const Expr *literal, const Expr *other, const Type *hint,
                                       bool bitwise) {
  const Type *type = other->type ? other->type->nonOptional() : nullptr;
  if (type && (type->isInteger() || (type->isFloat() && !bitwise)) &&
      BinaryChecker::onlyIntLiterals(literal) && !TypeRelations::isIntLiteralTree(other)) {
    return type;
  }

  return hint;
}

BinaryChecker::BinaryChecker(Checker &owner)
    : checker(owner) {}

void BinaryChecker::reportOperandTypes(llvm::SMLoc loc, const char *op, const Type *lhs,
                                       const Type *rhs) {
  std::string message = (llvm::Twine("operator '") + op + "' cannot be applied to types '" +
                         lhs->name() + "' and '" + rhs->name() + "'")
                            .str();
  if (lhs->isOptional() || rhs->isOptional() || lhs->isUndefined() || rhs->isUndefined()) {
    message += "; a value may be undefined: check it first or give a default with '??'";
  }
  this->checker.diags_.error(loc, message);
}

Expr *BinaryChecker::checkBinary(BinaryExpr *bin, const Type *expected, bool wraps) {
  const char *op = opSpelling(bin->op);
  if (expected) {
    expected = expected->nonOptional();
  }
  switch (bin->op) {
  case BinaryOp::In:
    return this->checker.unaryChecker().checkIn(bin);

  case BinaryOp::InstanceOf:
    return this->checker.instanceOfChecker().checkInstanceOf(bin);

  case BinaryOp::LogAnd:
  case BinaryOp::LogOr: {
    Checker::Flow whenTrue, whenFalse;
    this->checker.truthChecker().checkLogical(bin, expected, whenTrue, whenFalse, false,
                                              bin->op == BinaryOp::LogAnd ? Checker::TruthUse::And
                                                                          : Checker::TruthUse::Or);
    this->checker.flow_ = FlowChecker::join(whenTrue, whenFalse);
    return bin;
  }

  case BinaryOp::Nullish:
    return this->checkNullish(bin, expected);

  case BinaryOp::Eq:
  case BinaryOp::Ne:
    return this->checkEquality(bin);

  case BinaryOp::Lt:
  case BinaryOp::Le:
  case BinaryOp::Gt:
  case BinaryOp::Ge: {
    this->checker.decimalChecker().checkComparisonOperands(bin);
    this->checker.resultTypes().neverOperands(bin);
    bin->type = Type::boolTy();
    if (bin->lhs->type->isParam() && bin->lhs->type == bin->rhs->type) {
      return bin;
    }
    bin->lhs = this->checker.genericChecker().apparent(bin->lhs);
    bin->rhs = this->checker.genericChecker().apparent(bin->rhs);
    const Type *l = bin->lhs->type;
    const Type *r = bin->rhs->type;
    if (l->isError() || r->isError()) {
      return bin;
    }
    if (l->isString() && r->isString()) {
      return bin;
    }
    if (Expr *dates = this->checker.dateChecker().checkDateOperands(bin)) {
      return dates;
    }
    if (l->isDecimal() || r->isDecimal()) {
      return this->checker.decimalChecker().checkDecimalComparison(bin);
    }
    if (l->isNumeric() && r->isNumeric()) {
      this->adaptLiterals(bin);
      l = bin->lhs->type;
      r = bin->rhs->type;
      if (l == r || (l->isInteger() && r->isInteger())) {
        return bin;
      }
      this->reportMixedNumbers(bin);
      return bin;
    }
    this->reportOperandTypes(bin->loc(), op, l, r);
    return bin;
  }

  case BinaryOp::Shl:
  case BinaryOp::Shr:
  case BinaryOp::UShr: {
    const Type *hint =
        expected && expected->nonOptional()->isInteger() ? expected->nonOptional() : nullptr;
    bin->lhs = this->checker.genericChecker().apparent(this->checker.exprChecker().checkExpr(
        bin->lhs, BinaryChecker::onlyIntLiterals(bin->lhs) ? hint : nullptr));
    bin->rhs =
        this->checker.genericChecker().apparent(this->checker.exprChecker().checkExpr(bin->rhs));
    return this->checkBits(bin);
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
    BinaryExpr *chain = std::exchange(this->checker.concatChain_, nullptr);
    bool chainEnd = bin->op == BinaryOp::Add && !chain;
    const BinaryExpr *outerMixed =
        chainEnd ? std::exchange(this->checker.mixedConcat_, nullptr) : nullptr;
    if (auto *left = dyn_cast<BinaryExpr>(bin->lhs);
        bin->op == BinaryOp::Add && left && left->op == BinaryOp::Add) {
      this->checker.concatChain_ = chain ? chain : bin;
    }
    bool literals = !bitwise && !pow;
    if (DecimalChecker::isLiteralTree(bin->lhs) && !DecimalChecker::isLiteralTree(bin->rhs) &&
        (literals || BinaryChecker::onlyIntLiterals(bin->lhs))) {
      BinaryExpr *pending = std::exchange(this->checker.concatChain_, nullptr);
      bin->rhs = this->checker.genericChecker().apparent(
          this->checker.exprChecker().checkExpr(bin->rhs, rhsHint, operandsWrap));
      this->checker.concatChain_ = pending;
      const Type *lhsHint = BinaryChecker::integerHint(
          bin->lhs, bin->rhs, literals ? DecimalChecker::literalHint(bin->rhs, hint) : hint,
          bitwise);
      bin->lhs = this->checker.genericChecker().apparent(
          this->checker.exprChecker().checkExpr(bin->lhs, lhsHint, operandsWrap));
      this->checker.concatChain_ = nullptr;
    } else {
      bin->lhs = this->checker.genericChecker().apparent(
          this->checker.exprChecker().checkExpr(bin->lhs, hint, operandsWrap));
      this->checker.concatChain_ = nullptr;
      if (literals && DecimalChecker::isLiteralTree(bin->rhs)) {
        rhsHint = DecimalChecker::literalHint(bin->lhs, rhsHint);
      }
      rhsHint = BinaryChecker::integerHint(bin->rhs, bin->lhs, rhsHint, bitwise);
      bin->rhs = this->checker.genericChecker().apparent(
          this->checker.exprChecker().checkExpr(bin->rhs, rhsHint, operandsWrap));
    }
    this->checker.resultTypes().neverOperands(bin);
    this->adaptLiterals(bin);
    const Type *l = bin->lhs->type;
    const Type *r = bin->rhs->type;
    if (bin->op == BinaryOp::Add) {
      auto joinsAsText = [](const Type *type) {
        const Type *base = type->nonOptional();

        return base->isNumeric() || base->isDecimal() || base->isBool();
      };
      if (!this->checker.mixedConcat_ &&
          ((l->isString() && joinsAsText(r)) || (r->isString() && joinsAsText(l)))) {
        this->checker.mixedConcat_ = bin;
      }
      if (chainEnd) {
        if (this->checker.mixedConcat_) {
          this->checker.concatChecker().reportMixedConcat(bin, this->checker.mixedConcat_);
        }
        this->checker.mixedConcat_ = outerMixed;
      }
    }
    bin->type = Type::error();
    if (l->isError() || r->isError()) {
      return bin;
    }
    if (bin->op == BinaryOp::Add && (l->isString() || r->isString())) {
      bin->lhs = this->checker.concatChecker().toStringOperand(bin->lhs);
      bin->rhs = this->checker.concatChecker().toStringOperand(bin->rhs);
      bin->type = Type::string();
      return bin;
    }
    if (Expr *dates = this->checker.dateChecker().checkDateOperands(bin)) {
      return dates;
    }
    if (bitwise) {
      return this->checkBits(bin);
    }
    if (l->isDecimal() || r->isDecimal()) {
      return this->checker.decimalChecker().checkDecimalArithmetic(bin);
    }
    if ((l->isWrapping() || r->isWrapping()) && l->isInteger() && r->isInteger()) {
      return this->checkWrapping(bin);
    }
    if (bin->op == BinaryOp::Pow && l->isFloat() && r->isInteger()) {
      bin->rhs = this->checker.typeRelations().convert(bin->rhs, l);
      r = l;
    }
    bool operandsOk = bitwise ? l->isInteger() && r->isInteger() : l->isNumeric() && r->isNumeric();
    const Type *common = operandsOk ? TypeRelations::commonNumericType(l, r) : nullptr;
    if (!common && operandsOk) {
      this->reportMixedNumbers(bin);
      return bin;
    }
    if (!common) {
      this->reportOperandTypes(bin->loc(), op, l, r);
      return bin;
    }
    bin->lhs = this->checker.typeRelations().convert(bin->lhs, common);
    bin->rhs = this->checker.typeRelations().convert(bin->rhs, common);
    bin->type = common;
    if (!common->isInteger()) {
      return bin;
    }
    if (bin->op == BinaryOp::Pow) {
      if (std::optional<int64_t> exponent = this->checker.constantFolder().intConstant(bin->rhs);
          exponent && *exponent < 0) {
        this->checker.diags_.error(bin->rhs->loc(),
                                   "'" + common->name() +
                                       "' raised to a negative power is a fraction, which an "
                                       "integer cannot hold; use a double, e.g. '2.0 ** -1'");
        return bin;
      }
    }
    ConstantFolder::ResultDivisorNotZero divisor =
        this->checker.constantFolder().checkDivisor(bin->op, bin->rhs);
    if (divisor.isErr()) {
      this->checker.diags_.error(bin->rhs->loc(), divisor.error().message);
    }
    this->checker.constantFolder().foldBinary(bin, wraps);
    return bin;
  }
  }
}

Expr *BinaryChecker::checkWrapping(BinaryExpr *bin) {
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
  if (l->isWrapping() && r->isWrapping() && l != r) {
    this->reportMixedNumbers(bin);
    return bin;
  }
  const Type *wrap = l->isWrapping() ? l : r;
  bin->lhs = this->checker.typeRelations().convert(bin->lhs, wrap);
  bin->rhs = this->checker.typeRelations().convert(bin->rhs, wrap);
  bin->type = wrap;
  ConstantFolder::ResultDivisorNotZero divisor =
      this->checker.constantFolder().checkDivisor(bin->op, bin->rhs);
  if (divisor.isErr()) {
    this->checker.diags_.error(bin->rhs->loc(), divisor.error().message);
  }

  return bin;
}

Expr *BinaryChecker::checkBits(BinaryExpr *bin) {
  const char *op = opSpelling(bin->op);
  bool shift = bin->op == BinaryOp::Shl || bin->op == BinaryOp::Shr || bin->op == BinaryOp::UShr;
  this->adaptLiterals(bin);
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
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
    if (l->isFloat() && rounded && isRoundingBuiltin(rounded->builtin())) {
      message += "; to make it an int, write '" + std::string(lhsText.str()) + ".toInt()'";
    } else if (l->isFloat()) {
      message +=
          "; to drop the fraction, write 'Math.trunc(" + std::string(lhsText.str()) + ").toInt()'";
    } else {
      message += "; to make it an int32, write '" + text + ".toInt32()'" +
                 (bin->op == BinaryOp::UShr ? ", a uint32 '" + text + ".toUint32()'" : "") +
                 ", and to count modulo 2^32, use 'Bit.Wrap32'";
    }
    this->checker.diags_.error(bin->loc(), message);
    return bin;
  }
  if (l->isFloat() || r->isFloat()) {
    std::string side = l->isFloat()      ? text
                       : rhsText.empty() ? "x"
                                         : TypeRelations::operandText(rhsText);
    this->checker.diags_.error(
        bin->loc(), llvm::Twine("operator '") + op +
                        "' works on the bits of integers, and this is a double; drop the "
                        "fraction first, e.g. 'Math.trunc(" +
                        side + ").toInt()'");
    return bin;
  }
  if (!l->isInteger() || !r->isInteger()) {
    this->reportOperandTypes(bin->loc(), op, l, r);
    return bin;
  }
  if (shift) {
    if (bin->op == BinaryOp::UShr && !l->isUnsigned()) {
      std::string count = rhsText.empty() ? "n" : rhsText.str();
      this->checker.diags_.error(
          bin->loc(), "'>>>' shifts in zeros, as a shift of an unsigned type does; '" + l->name() +
                          "' is signed: use '>>', or convert it, e.g. '" + text + "." +
                          TypeRelations::conversionMethod(Type::integer(l->bitWidth(), true)) +
                          "() >>> " + count + "'");
      return bin;
    }
    bin->rhs = this->checker.typeRelations().convert(bin->rhs, l);
    bin->type = l;
    this->checker.constantFolder().foldBinary(bin, false);
    return bin;
  }
  if (l->isWrapping() || r->isWrapping()) {
    return this->checkWrapping(bin);
  }
  if (l != r) {
    this->reportMixedNumbers(bin);
    return bin;
  }
  bin->type = l;
  this->checker.constantFolder().foldBinary(bin, false);

  return bin;
}

void BinaryChecker::adaptLiterals(BinaryExpr *bin) {
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
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
  if (this->checker.typeRelations().literalAdapts(bin->lhs, r) &&
      !(bin->op == BinaryOp::Pow && r->isDecimal())) {
    bin->lhs = this->checker.typeRelations().adaptLiteral(bin->lhs, r);
  } else if (this->checker.typeRelations().literalAdapts(bin->rhs, l) &&
             !(bin->op == BinaryOp::Pow && (l->isDecimal() || l->isFloat()))) {
    bin->rhs = this->checker.typeRelations().adaptLiteral(bin->rhs, l);
  }
}

void BinaryChecker::reportMixedNumbers(const BinaryExpr *bin) {
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
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
  this->checker.diags_.error(bin->loc(), message);
}

bool BinaryChecker::checkTypeOfName(const Expr *operand, const StringLitExpr *text) {
  std::string name = toUtf8(text->value);
  auto quoted = [](llvm::ArrayRef<llvm::StringRef> names) {
    std::string list;
    for (size_t i = 0; i < names.size(); ++i) {
      list += (i == 0 ? "" : i + 1 == names.size() ? " or " : ", ") + ("\"" + names[i].str() + "\"");
    }
    return list;
  };
  if (name == "bigint") {
    this->checker.decimalChecker().reportBigInt(text->loc(), "'typeof x == \"bigint\"'");
    return false;
  }
  if (!isTypeOfName(name)) {
    std::vector<llvm::StringRef> all(std::begin(kTypeOfNames), std::end(kTypeOfNames));
    if (name == "number") {
      std::vector<llvm::StringRef> numbers;
      if (operand->type && !operand->type->isError()) {
        for (llvm::StringRef possible : typeOfNames(operand->type)) {
          if (possible.starts_with("int") || possible.starts_with("uint") ||
              possible == "double" || possible == "decimal") {
            numbers.push_back(possible);
          }
        }
      }
      std::string here = numbers.empty() || numbers.size() > 3
                             ? std::string()
                             : "; for a value of type '" + operand->type->name().str() +
                                   "' it is " + quoted(numbers);
      this->checker.diags_.error(
          text->loc(), "typeof gives no \"number\": each number type has its own name, "
                       "\"int\", \"double\", \"decimal\", \"int32\"...; the type 'number' is "
                       "'double', and its typeof is \"double\"" +
                           here);
      return false;
    }
    std::string why = name == "symbol" ? "typeof gives no \"symbol\": tz has no symbols as values"
                                       : "'" + name + "' is not a name that typeof gives";
    this->checker.diags_.error(text->loc(), why + "; the names are " + quoted(all));
    return false;
  }
  const Type *type = operand->type;
  if (!type || type->isError() || this->checker.checkingInstance_ ||
      type->nonOptional()->isParam()) {
    return true;
  }
  std::vector<llvm::StringRef> names = typeOfNames(type);
  if (names.empty() || llvm::is_contained(names, llvm::StringRef(name))) {
    return true;
  }
  this->checker.diags_.error(text->loc(), "typeof of a value of type '" + type->name() + "' is " +
                                              quoted(names) + ", never \"" + name + "\"");

  return false;
}

Expr *BinaryChecker::checkEquality(BinaryExpr *bin) {
  if (Expr *null = this->checker.jsonChecker().checkNullEquality(bin)) {
    return null;
  }
  this->checker.decimalChecker().checkComparisonOperands(bin);
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
  bin->type = Type::boolTy();
  for (auto [side, other] : {std::pair{bin->lhs, bin->rhs}, std::pair{bin->rhs, bin->lhs}}) {
    auto *typeOf = dyn_cast<UnaryExpr>(side);
    auto *text = dyn_cast<StringLitExpr>(other);
    if (typeOf && typeOf->op == UnaryOp::TypeOf && text) {
      this->checkTypeOfName(typeOf->operand, text);
    }
  }
  if (l->isError() || r->isError()) {
    return bin;
  }
  if (l->isVoid() || r->isVoid()) {
    this->reportOperandTypes(bin->loc(), opSpelling(bin->op), l, r);
    return bin;
  }
  if (l->isUndefined() || r->isUndefined()) {
    return bin;
  }
  if (l->nonOptional()->isJson() || r->nonOptional()->isJson()) {
    return this->checker.jsonChecker().checkJsonEquality(bin);
  }
  if (l->nonOptional()->isResult() || r->nonOptional()->isResult()) {
    this->checker.diags_.error(
        bin->loc(), llvm::Twine("Results cannot be compared with '") + opSpelling(bin->op) +
                        "': in JavaScript 'ok(1) == ok(1)' is false, as they are two "
                        "objects; compare what they hold, with 'isOk()', 'match' or "
                        "'value'");
    return bin;
  }
  if (l->nonOptional()->isParam() || r->nonOptional()->isParam()) {
    return bin;
  }

  this->adaptLiterals(bin);
  l = bin->lhs->type;
  r = bin->rhs->type;
  if (l->isInteger() && r->isInteger()) {
    return bin;
  }
  const Type *common = TypeRelations::comparisonType(l, r);
  if (!common) {
    if (l->nonOptional()->isDecimal() || r->nonOptional()->isDecimal()) {
      this->checker.decimalChecker().reportDecimalMix(bin);
    } else if (l->nonOptional()->isNumeric() && r->nonOptional()->isNumeric() && !l->isOptional() &&
               !r->isOptional()) {
      this->reportMixedNumbers(bin);
    } else {
      this->reportOperandTypes(bin->loc(), opSpelling(bin->op), l, r);
    }
    return bin;
  }
  if (common->nonOptional()->isFunction()) {
    this->checker.file_->comparesFunctions = true;
  }
  bin->lhs = this->checker.typeRelations().convert(bin->lhs, common);
  bin->rhs = this->checker.typeRelations().convert(bin->rhs, common);

  return bin;
}

Expr *BinaryChecker::checkNullish(BinaryExpr *bin, const Type *expected) {
  bin->lhs = this->checker.exprChecker().checkExpr(bin->lhs, expected);
  if (bin->lhs->type->nonOptional()->isMatch() && isa<ArrayLitExpr>(bin->rhs)) {
    bin->lhs = this->checker.matchChecker().matchItems(bin->lhs);
  }
  const Type *l = bin->lhs->type;
  Checker::Flow afterLhs = this->checker.flow_;
  const Type *hint = l->isUndefined() || l->isError() ? expected : l->nonOptional();
  bin->rhs = this->checker.exprChecker().checkExpr(bin->rhs, hint);
  this->checker.flow_ = FlowChecker::join(afterLhs, this->checker.flow_);
  const Type *r = bin->rhs->type;
  bin->type = Type::error();
  if (l->isError() || r->isError()) {
    if (this->checker.trial_ && l->isError() != r->isError()) {
      bin->type = l->isError() ? r : l->nonOptional();
    }
    return bin;
  }
  if (l->isVoid() || r->isVoid()) {
    this->reportOperandTypes(bin->loc(), "??", l, r);
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
    this->reportOperandTypes(bin->loc(), "??", l, r);
    return bin;
  }
  const Type *result = r->isOptional() || r->isUndefined() ? Type::optional(base) : base;
  bin->lhs = this->checker.typeRelations().convert(bin->lhs,
                                                   l->isOptional() ? Type::optional(base) : result);
  bin->rhs = this->checker.typeRelations().convert(bin->rhs, result);
  bin->type = result;

  return bin;
}

Expr *BinaryChecker::toInt32(Expr *expr) {
  return expr->type->isInt() || expr->type->isError()
             ? expr
             : this->checker.typeRelations().convert(expr, Type::intTy());
}

}
