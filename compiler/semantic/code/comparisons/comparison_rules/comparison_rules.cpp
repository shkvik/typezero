#include "compiler/semantic/code/comparisons/comparison_rules/comparison_rules.h"
#include "compiler/semantic/bodies/shared_helpers/shared_helpers.h"
#include "compiler/semantic/code/arithmetic/code_arithmetic_registry_errors.h"
#include "compiler/semantic/code/operators/code_operators_registry_errors.h"
#include <cstring>

namespace tz {

ComparisonRules::ComparisonRules(SemanticState &owner)
    : state(owner) {}

void ComparisonRules::pushLiteralAdapting(BinaryExpr *binary) {
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  if (leftType == nullptr || rightType == nullptr || leftType == rightType || leftType->isError() ||
      rightType->isError()) {
    return;
  }
  bool isBitwise = this->isBitwiseOperator(binary->op) || this->isShiftOperator(binary->op);
  bool hasNumbers = this->isNumberLike(leftType) && this->isNumberLike(rightType);
  bool hasIntegers = leftType->isInteger() && rightType->isInteger();
  if (!hasNumbers || (isBitwise && !hasIntegers)) {
    return;
  }
  const auto &conversions = this->state.code.conversions;
  bool isPower = binary->op == BinaryOp::Pow;
  bool isLeftAdapting =
      conversions.isLiteralAdapting(binary->lhs, rightType) && !(isPower && rightType->isDecimal());
  if (isLeftAdapting) {
    return this->state.walk.tasks.pushTask(AdaptLiteralSlot{.slot = &binary->lhs, .to = rightType});
  }
  bool isRightAdapting = conversions.isLiteralAdapting(binary->rhs, leftType) &&
                         !(isPower && (leftType->isDecimal() || leftType->isFloat()));
  if (isRightAdapting) {
    this->state.walk.tasks.pushTask(AdaptLiteralSlot{.slot = &binary->rhs, .to = leftType});
  }
}

std::string ComparisonRules::operandTypesText(llvm::StringRef op, const Type *lhs,
                                              const Type *rhs) const {
  bool isUndefined =
      lhs->isOptional() || rhs->isOptional() || lhs->isUndefined() || rhs->isUndefined();

  return ErrorTextOperands::types(op, lhs->name(), rhs->name(), isUndefined);
}

bool ComparisonRules::isBitwiseOperator(BinaryOp op) const {
  return op == BinaryOp::BitAnd || op == BinaryOp::BitOr || op == BinaryOp::BitXor;
}

bool ComparisonRules::isShiftOperator(BinaryOp op) const {
  return op == BinaryOp::Shl || op == BinaryOp::Shr || op == BinaryOp::UShr;
}

bool ComparisonRules::isNumberLike(const Type *type) const {
  return type->isNumeric() || type->isDecimal();
}

std::string ComparisonRules::mixedNumbersText(const BinaryExpr *binary) const {
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  const char *op = Operators::opSpelling(binary->op);
  auto conversion = this->mixedConversion(leftType, rightType);
  auto example = this->mixedExample(binary, conversion);

  return ErrorTextMixedNumbers::mixed(op, leftType->name(), rightType->name(), example);
}

ComparisonRules::NumberConversion ComparisonRules::mixedConversion(const Type *leftType,
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

std::string ComparisonRules::mixedExample(const BinaryExpr *binary,
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

std::string ComparisonRules::convertedText(llvm::StringRef operand,
                                           const std::string &method) const {
  return this->state.code.conversions.operandText(operand) + "." + method + "()";
}

}
