#include "compiler/semantic/code/arithmetic/arithmetic_rules/arithmetic_rules.h"
#include "compiler/semantic/bodies/shared_helpers/shared_helpers.h"
#include "compiler/semantic/code/operators/code_operators_registry_errors.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/CheckedArithmetic.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

using llvm::dyn_cast;

namespace tz {

namespace {

constexpr double kNoExactBelow = 1e21;
constexpr unsigned kWideBits = 128;

}

ArithmeticRules::ArithmeticRules(SemanticState &owner)
    : state(owner) {}

void ArithmeticRules::startWrapping(BinaryExpr *binary) {
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  if (leftType->isWrapping() && rightType->isWrapping() && leftType != rightType) {
    auto message = this->mixedNumbersText(binary);
    this->state.diagnostics.error(binary->loc(), message);
    return this->state.walk.tasks.pushChecked(binary);
  }
  const Type *wrappingType = leftType->isWrapping() ? leftType : rightType;
  binary->type = wrappingType;
  this->state.walk.tasks.pushChecked(binary);
  this->state.walk.tasks.pushTask(CheckWrappingDivisor{.binary = binary});
  this->state.walk.tasks.pushTask(ConvertSlot{.slot = &binary->rhs, .to = wrappingType});

  return this->state.walk.tasks.pushTask(ConvertSlot{.slot = &binary->lhs, .to = wrappingType});
}

Expr *ArithmeticRules::foldedUnary(UnaryExpr *unary) {
  if (unary->type != nullptr && unary->type->isInteger()) {
    this->foldUnary(unary);
  }

  return unary;
}

std::string ArithmeticRules::operandTypesText(llvm::StringRef op, const Type *lhs,
                                              const Type *rhs) const {
  bool isUndefined =
      lhs->isOptional() || rhs->isOptional() || lhs->isUndefined() || rhs->isUndefined();

  return ErrorTextOperands::types(op, lhs->name(), rhs->name(), isUndefined);
}

bool ArithmeticRules::isShiftOperator(BinaryOp op) const {
  return op == BinaryOp::Shl || op == BinaryOp::Shr || op == BinaryOp::UShr;
}

std::string ArithmeticRules::mixedNumbersText(const BinaryExpr *binary) const {
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  const char *op = Operators::opSpelling(binary->op);
  auto conversion = this->mixedConversion(leftType, rightType);
  auto example = this->mixedExample(binary, conversion);

  return ErrorTextMixedNumbers::mixed(op, leftType->name(), rightType->name(), example);
}

ArithmeticRules::NumberConversion ArithmeticRules::mixedConversion(const Type *leftType,
                                                                   const Type *rightType) const {
  if (leftType->isFloat() || rightType->isFloat()) {
    return NumberConversion{
        .target = Type::doubleTy(),
        .isLeftConverted = !leftType->isFloat(),
    };
  }
  if (leftType->holds(rightType)) {
    return NumberConversion{
        .target = leftType,
        .isLeftConverted = false,
    };
  }
  if (rightType->holds(leftType)) {
    return NumberConversion{
        .target = rightType,
        .isLeftConverted = true,
    };
  }

  return NumberConversion{
      .target = Type::intTy(),
      .isLeftConverted = !leftType->isInt(),
  };
}

std::string ArithmeticRules::mixedExample(const BinaryExpr *binary,
                                          const NumberConversion &conversion) const {
  const char *op = Operators::opSpelling(binary->op);
  auto [lhs, rhs] = SharedHelpers::operandTexts(binary->source, binary->loc(),
                                                std::strlen(Operators::opSpelling(binary->op)));
  auto method = this->state.code.conversions.conversionMethod(conversion.target);
  if (lhs.empty() || rhs.empty()) {
    return ErrorTextMixedNumbers::method(method);
  }
  if (conversion.isLeftConverted) {
    auto left = this->convertedText(lhs, method);
    return ErrorTextMixedNumbers::example(left, op, rhs);
  }
  auto right = this->convertedText(rhs, method);

  return ErrorTextMixedNumbers::example(lhs, op, right);
}

std::string ArithmeticRules::convertedText(llvm::StringRef operand,
                                           const std::string &method) const {
  return this->state.code.conversions.operandText(operand) + "." + method + "()";
}

std::string ArithmeticRules::formatNumber(double value) const {
  char text[32];
  std::snprintf(text, sizeof text, "%.15g", value);

  return text;
}

int64_t ArithmeticRules::wrapTo(const Type *type, int64_t value) const {
  int bits = int(type->bitWidth());
  if (bits == 64) {
    return value;
  }
  uint64_t low = static_cast<uint64_t>(value) & ((uint64_t(1) << bits) - 1);
  if (type->isUnsigned()) {
    return static_cast<int64_t>(low);
  }
  uint64_t sign = uint64_t(1) << (bits - 1);

  return static_cast<int64_t>((low ^ sign) - sign);
}

bool ArithmeticRules::isFitting(const Type *type, int64_t value) const {
  if (value < type->intMin()) {
    return false;
  }

  return value < 0 || static_cast<uint64_t>(value) <= type->intMax();
}

std::optional<int64_t> ArithmeticRules::checkedProduct(std::optional<int64_t> left,
                                                       std::optional<int64_t> right) const {
  if (!left.has_value() || !right.has_value()) {
    return std::nullopt;
  }

  return llvm::checkedMul(*left, *right);
}

ArithmeticRules::IntPower ArithmeticRules::intPower(int64_t base, int64_t exponent) const {
  uint64_t wrapped = 1;
  auto factor = static_cast<uint64_t>(base);
  std::optional<int64_t> exact = 1;
  std::optional<int64_t> exactFactor = base;
  for (int64_t rest = exponent; rest > 0; rest >>= 1) {
    if ((rest & 1) != 0) {
      wrapped *= factor;
      exact = this->checkedProduct(exact, exactFactor);
    }
    factor *= factor;
    if (rest > 1) {
      exactFactor = this->checkedProduct(exactFactor, exactFactor);
    }
  }

  return IntPower{.wrapped = static_cast<int64_t>(wrapped), .exact = exact};
}

llvm::APInt ArithmeticRules::wideResult(BinaryOp op, const llvm::APInt &left,
                                        const llvm::APInt &right) const {
  if (op == BinaryOp::Add) {
    return left + right;
  }
  if (op == BinaryOp::Sub) {
    return left - right;
  }

  return left * right;
}

std::string ArithmeticRules::wideText(int64_t left, int64_t right, BinaryOp op) const {
  llvm::APInt wideLeft(kWideBits, static_cast<uint64_t>(left), true);
  llvm::APInt wideRight(kWideBits, static_cast<uint64_t>(right), true);
  auto wide = this->wideResult(op, wideLeft, wideRight);

  return this->decimalText(wide);
}

std::string ArithmeticRules::decimalText(const llvm::APInt &wide) const {
  auto digits = llvm::SmallString<48>();
  wide.toString(digits, 10, true);

  return std::string(digits);
}

std::string ArithmeticRules::constantText(std::optional<int64_t> exact, double value) const {
  if (exact.has_value()) {
    return std::to_string(*exact);
  }
  if (std::fabs(value) >= kNoExactBelow) {
    return this->formatNumber(value);
  }
  char text[32];
  std::snprintf(text, sizeof text, "%.0f", value);

  return text;
}

std::optional<uint64_t> ArithmeticRules::nonNegative(std::optional<int64_t> value) const {
  if (!value.has_value() || *value < 0) {
    return std::nullopt;
  }

  return uint64_t(*value);
}

bool ArithmeticRules::isUnsigned64(const Type *type) const {
  return type != nullptr && type->isUnsigned() && type->bitWidth() == 64;
}

std::optional<uint64_t> ArithmeticRules::unsignedConstant(const Expr *expr) const {
  if (!this->isUnsigned64(expr->type)) {
    auto value = this->state.code.constants.intConstant(expr);

    return this->nonNegative(value);
  }
  auto *literal = dyn_cast<IntLitExpr>(expr);
  auto *name = dyn_cast<NameExpr>(expr);
  auto &constants = this->state.code.constants;
  if (literal != nullptr) {
    return literal->value;
  }
  std::optional<uint64_t> bigVar =
      name != nullptr ? constants.findBigUnsignedVar(name->var()) : std::nullopt;
  if (bigVar.has_value()) {
    return bigVar;
  }
  auto big = constants.findBigUnsigned(expr);
  if (big.has_value()) {
    return big;
  }
  auto value = this->state.code.constants.intConstant(expr);

  return this->nonNegative(value);
}

void ArithmeticRules::foldUnary(UnaryExpr *unary) {
  if (unary->type->isWrapping()) {
    return;
  }
  auto *literal = dyn_cast<IntLitExpr>(unary->operand);
  if (literal != nullptr && unary->op == UnaryOp::Neg) {
    return this->foldNegativeLiteral(unary, literal);
  }
  auto value = this->state.code.constants.intConstant(unary->operand);
  if (!value.has_value()) {
    return;
  }
  switch (unary->op) {
    case UnaryOp::Plus:
      return this->state.code.constants.rememberIntConstant(unary, *value);
    case UnaryOp::BitNot:
      return this->foldBitNot(unary, *value);
    case UnaryOp::Neg:
      return this->foldNegation(unary, *value);
    case UnaryOp::Not:
    case UnaryOp::TypeOf:
    case UnaryOp::Void:
    case UnaryOp::Delete:
    case UnaryOp::NonNull:
      return;
  }
}

void ArithmeticRules::foldNegativeLiteral(UnaryExpr *unary, const IntLitExpr *literal) {
  uint64_t magnitude = literal->value;
  if (magnitude > uint64_t(1) << 63) {
    return;
  }
  auto negative = static_cast<int64_t>(0 - magnitude);
  if (this->isFitting(unary->type, negative)) {
    this->state.code.constants.rememberIntConstant(unary, negative);
  }
}

void ArithmeticRules::foldBitNot(UnaryExpr *unary, int64_t value) {
  const Type *type = unary->type;
  int64_t wrapped = this->wrapTo(type, value);
  int64_t inverted = this->wrapTo(type, ~wrapped);
  this->state.code.constants.rememberIntConstant(unary, inverted);
}

void ArithmeticRules::foldNegation(UnaryExpr *unary, int64_t value) {
  const Type *type = unary->type;
  std::optional<int64_t> exact = llvm::checkedSub(int64_t(0), value);
  if (exact.has_value() && this->isFitting(type, *exact)) {
    this->state.code.constants.rememberIntConstant(unary, *exact);
    return;
  }
  std::string shown =
      exact.has_value() ? std::to_string(*exact) : this->wideText(0, value, BinaryOp::Sub);
  auto negated = static_cast<int64_t>(0 - static_cast<uint64_t>(value));
  int64_t wrapped = this->wrapTo(type, negated);
  auto message = this->overflowText(type, shown, wrapped);
  this->state.diagnostics.error(unary->loc(), message);
}

void ArithmeticRules::foldBinary(BinaryExpr *binary) {
  const Type *type = binary->type;
  if (type->isWrapping() || binary->lhs->type->isWrapping()) {
    return;
  }
  if (this->isUnsigned64(type)) {
    return this->foldUnsigned64(binary);
  }
  auto left = this->state.code.constants.intConstant(binary->lhs);
  auto right = this->state.code.constants.intConstant(binary->rhs);
  if (!left.has_value() || !right.has_value()) {
    return;
  }
  auto folded = this->foldIntOp(binary, *left, *right);
  if (folded.has_value()) {
    this->rememberFolded(binary, *folded);
  }
}

void ArithmeticRules::foldUnsigned64(BinaryExpr *binary) {
  auto left = this->unsignedConstant(binary->lhs);
  auto right = this->unsignedConstant(binary->rhs);
  if (!left.has_value() || !right.has_value()) {
    return;
  }
  bool isSupported =
      binary->op == BinaryOp::Add || binary->op == BinaryOp::Sub || binary->op == BinaryOp::Mul;
  if (!isSupported) {
    return;
  }
  auto result = this->unsignedResult(binary->op, *left, *right);
  if (!result.has_value()) {
    return this->reportUnsignedOverflow(binary, *left, *right);
  }
  if (*result <= uint64_t(std::numeric_limits<int64_t>::max())) {
    this->state.code.constants.rememberIntConstant(binary, static_cast<int64_t>(*result));
    return;
  }
  this->state.code.constants.rememberBigUnsigned(binary, *result);
}

void ArithmeticRules::reportUnsignedOverflow(const BinaryExpr *binary, uint64_t left,
                                             uint64_t right) {
  llvm::APInt wideLeft(kWideBits, left);
  llvm::APInt wideRight(kWideBits, right);
  auto wide = this->wideResult(binary->op, wideLeft, wideRight);
  auto shown = this->decimalText(wide);
  auto message = this->overflowText(binary->type, shown, 0);
  this->state.diagnostics.error(binary->loc(), message);
}

std::optional<uint64_t> ArithmeticRules::unsignedResult(BinaryOp op, uint64_t left,
                                                        uint64_t right) const {
  if (op == BinaryOp::Add) {
    return llvm::checkedAddUnsigned(left, right);
  }
  if (op == BinaryOp::Sub && left < right) {
    return std::nullopt;
  }
  if (op == BinaryOp::Sub) {
    return left - right;
  }

  return llvm::checkedMulUnsigned(left, right);
}

std::optional<ArithmeticRules::IntFold> ArithmeticRules::foldIntOp(BinaryExpr *binary, int64_t left,
                                                                   int64_t right) {
  switch (binary->op) {
    case BinaryOp::Add:
    case BinaryOp::Sub:
    case BinaryOp::Mul:
      return this->foldArithmetic(binary->op, left, right);
    case BinaryOp::Div:
    case BinaryOp::Rem:
      return this->foldDivision(binary->op, binary->type, left, right);
    case BinaryOp::Pow:
      return this->foldPower(left, right);
    case BinaryOp::BitAnd:
    case BinaryOp::BitOr:
    case BinaryOp::BitXor:
      return this->foldBits(binary->op, binary->type, left, right);
    case BinaryOp::Shl:
    case BinaryOp::Shr:
    case BinaryOp::UShr:
      return this->foldShift(binary, left, right);
    case BinaryOp::Lt:
    case BinaryOp::Le:
    case BinaryOp::Gt:
    case BinaryOp::Ge:
    case BinaryOp::Eq:
    case BinaryOp::Ne:
    case BinaryOp::LogAnd:
    case BinaryOp::LogOr:
    case BinaryOp::Nullish:
    case BinaryOp::In:
    case BinaryOp::InstanceOf:
      return std::nullopt;
  }

  return std::nullopt;
}

ArithmeticRules::IntFold ArithmeticRules::exactFold(int64_t value) const {
  return IntFold{.exact = value, .wrapped = 0, .value = 0, .text = ""};
}

ArithmeticRules::IntFold ArithmeticRules::foldArithmetic(BinaryOp op, int64_t left,
                                                         int64_t right) const {
  auto unsignedLeft = static_cast<uint64_t>(left);
  auto unsignedRight = static_cast<uint64_t>(right);
  auto text = this->wideText(left, right, op);
  if (op == BinaryOp::Add) {
    return IntFold{
        .exact = llvm::checkedAdd(left, right),
        .wrapped = static_cast<int64_t>(unsignedLeft + unsignedRight),
        .value = 0,
        .text = text,
    };
  }
  if (op == BinaryOp::Sub) {
    return IntFold{
        .exact = llvm::checkedSub(left, right),
        .wrapped = static_cast<int64_t>(unsignedLeft - unsignedRight),
        .value = 0,
        .text = text,
    };
  }

  return IntFold{
      .exact = llvm::checkedMul(left, right),
      .wrapped = static_cast<int64_t>(unsignedLeft * unsignedRight),
      .value = 0,
      .text = text,
  };
}

std::optional<ArithmeticRules::IntFold>
ArithmeticRules::foldDivision(BinaryOp op, const Type *type, int64_t left, int64_t right) const {
  if (right == 0) {
    return std::nullopt;
  }
  if (right == -1 && !type->isUnsigned()) {
    return this->divisionByMinusOne(op, type, left);
  }
  int64_t quotient = op == BinaryOp::Rem ? left % right : left / right;

  return IntFold{
      .exact = quotient,
      .wrapped = quotient,
      .value = 0,
      .text = "",
  };
}

ArithmeticRules::IntFold ArithmeticRules::divisionByMinusOne(BinaryOp op, const Type *type,
                                                             int64_t left) const {
  auto text = this->wideText(0, left, BinaryOp::Sub);
  if (op == BinaryOp::Rem) {
    return IntFold{
        .exact = 0,
        .wrapped = 0,
        .value = 0,
        .text = text,
    };
  }
  int64_t min = type->intMin();
  int64_t wrapped = left == min ? min : -left;

  return IntFold{
      .exact = llvm::checkedSub(int64_t(0), left),
      .wrapped = wrapped,
      .value = 0,
      .text = text,
  };
}

std::optional<ArithmeticRules::IntFold> ArithmeticRules::foldPower(int64_t base,
                                                                   int64_t exponent) const {
  if (exponent < 0) {
    return std::nullopt;
  }
  auto product = this->intPower(base, exponent);
  double value = std::pow(static_cast<double>(base), static_cast<double>(exponent));

  return IntFold{
      .exact = product.exact,
      .wrapped = product.wrapped,
      .value = value,
      .text = "",
  };
}

ArithmeticRules::IntFold ArithmeticRules::foldBits(BinaryOp op, const Type *type, int64_t left,
                                                   int64_t right) const {
  int64_t wrappedLeft = this->wrapTo(type, left);
  int64_t wrappedRight = this->wrapTo(type, right);
  if (op == BinaryOp::BitAnd) {
    return this->exactFold(wrappedLeft & wrappedRight);
  }
  if (op == BinaryOp::BitOr) {
    return this->exactFold(wrappedLeft | wrappedRight);
  }

  return this->exactFold(wrappedLeft ^ wrappedRight);
}

uint64_t ArithmeticRules::bitPattern(int64_t value, int width) const {
  auto bits = static_cast<uint64_t>(value);
  if (width == 64) {
    return bits;
  }

  return bits & ((uint64_t(1) << width) - 1);
}

std::optional<ArithmeticRules::IntFold> ArithmeticRules::foldShift(BinaryExpr *binary,
                                                                   int64_t value, int64_t count) {
  const Type *shifted = binary->lhs->type;
  int width = int(shifted->bitWidth());
  auto counted = this->shiftCount(shifted, count);
  if (counted.isErr()) {
    this->state.diagnostics.error(binary->rhs->loc(), counted.error().message);
  }
  if (count < 0 || count >= int64_t(width)) {
    return std::nullopt;
  }
  int bits = int(count);
  int64_t wrapped = this->wrapTo(shifted, value);
  uint64_t pattern = this->bitPattern(wrapped, width);
  if (binary->op == BinaryOp::Shl) {
    return this->foldShiftLeft(binary, value, bits, pattern);
  }
  if (binary->op == BinaryOp::Shr && !shifted->isUnsigned()) {
    return this->exactFold(wrapped >> bits);
  }
  uint64_t moved = pattern >> bits;
  if (moved > uint64_t(std::numeric_limits<int64_t>::max())) {
    return std::nullopt;
  }

  return this->exactFold(static_cast<int64_t>(moved));
}

std::optional<ArithmeticRules::IntFold>
ArithmeticRules::foldShiftLeft(BinaryExpr *binary, int64_t value, int count, uint64_t pattern) {
  const Type *shifted = binary->lhs->type;
  int width = int(shifted->bitWidth());
  llvm::APInt start(kWideBits, uint64_t(value), !shifted->isUnsigned());
  llvm::APInt wide = start.shl(unsigned(count));
  int bitsUsed = shifted->isUnsigned() ? int(wide.getActiveBits()) : int(wide.getSignificantBits());
  if (bitsUsed > width && !shifted->isWrapping()) {
    auto shown = this->decimalText(wide);
    auto message = this->overflowText(binary->type, shown, 0);
    this->state.diagnostics.error(binary->loc(), message);

    return std::nullopt;
  }
  auto moved = static_cast<int64_t>(pattern << count);

  return this->exactFold(this->wrapTo(shifted, moved));
}

void ArithmeticRules::rememberFolded(BinaryExpr *binary, const IntFold &fold) {
  const Type *type = binary->type;
  if (fold.exact.has_value() && this->isFitting(type, *fold.exact)) {
    this->state.code.constants.rememberIntConstant(binary, *fold.exact);
    return;
  }
  std::string shown = fold.text.empty() ? this->constantText(fold.exact, fold.value) : fold.text;
  int64_t wrappedValue = this->wrapTo(type, fold.wrapped);
  auto message = this->overflowText(type, shown, wrappedValue);
  this->state.diagnostics.error(binary->loc(), message);
}

ArithmeticRules::ResultShiftCountFits ArithmeticRules::shiftCount(const Type *shifted,
                                                                  int64_t count) const {
  int width = int(shifted->bitWidth());
  bool isOutside = count < 0 || count >= int64_t(width);
  if (isOutside && !shifted->isWrapping()) {
    auto message = ErrorTextShiftCount::outside(count, width - 1, shifted->name());

    return err(CodeArithmeticRegistryShiftCountError{
        .code = CodeArithmeticRegistryShiftCountErrorCode::OUTSIDE_BITS,
        .message = message,
    });
  }

  return ok(shifted);
}

std::string ArithmeticRules::overflowText(const Type *type, const std::string &value,
                                          int64_t wrapped) const {
  if (type->bitWidth() < 64) {
    return ErrorTextOverflow::inInt(value, type->name(), wrapped);
  }

  return ErrorTextOverflow::inDouble(value, type->name(), wrapped);
}

}
