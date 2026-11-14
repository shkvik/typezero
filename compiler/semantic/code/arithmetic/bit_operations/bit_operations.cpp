#include "compiler/semantic/code/arithmetic/bit_operations/bit_operations.h"
#include "compiler/semantic/bodies/shared_helpers/shared_helpers.h"
#include "compiler/semantic/code/operators/code_operators_registry_errors.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/CheckedArithmetic.h"
#include <cstdio>
#include <cstring>
#include <string>

using llvm::dyn_cast;

namespace tz {

namespace {

constexpr const char *kValuePlaceholder = "x";
constexpr const char *kCountPlaceholder = "n";

}

BitOperations::BitOperations(SemanticState &owner, ArithmeticRules &sharedArithmeticRules)
    : state(owner),
      arithmeticRules(sharedArithmeticRules) {}

void BitOperations::finishBits(const FinishBits &task) {
  BinaryExpr *binary = task.binary;
  bool isShift = this->arithmeticRules.isShiftOperator(binary->op);
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  binary->type = Type::error();
  if (leftType->isError() || rightType->isError()) {
    return this->state.walk.tasks.pushChecked(binary);
  }
  auto problem = this->bitsProblem(binary);
  if (problem.has_value()) {
    auto message = this->bitsProblemText(binary, *problem);
    bool isDivisor = *problem == CodeArithmeticRegistryCheckBitsErrorCode::DIVISOR_ZERO;
    llvm::SMLoc at = isDivisor ? binary->rhs->loc() : binary->loc();
    this->state.diagnostics.error(at, message);
    return this->state.walk.tasks.pushChecked(binary);
  }
  if (isShift) {
    binary->type = leftType;
    this->state.walk.tasks.pushChecked(binary);
    this->state.walk.tasks.pushTask(FoldBinaryNode{.binary = binary});
    return this->state.walk.tasks.pushTask(ConvertSlot{.slot = &binary->rhs, .to = leftType});
  }
  if (leftType->isWrapping() || rightType->isWrapping()) {
    return this->arithmeticRules.startWrapping(binary);
  }
  if (leftType != rightType) {
    auto message = this->arithmeticRules.mixedNumbersText(binary);
    this->state.diagnostics.error(binary->loc(), message);
    return this->state.walk.tasks.pushChecked(binary);
  }
  binary->type = leftType;
  this->arithmeticRules.foldBinary(binary);

  return this->state.walk.tasks.pushChecked(binary);
}

void BitOperations::checkBitNot(const CheckBitNot &task) {
  UnaryExpr *unary = task.unary;
  const Type *wanted = task.expected != nullptr ? task.expected->nonOptional() : nullptr;
  const Type *hint = wanted != nullptr && wanted->isInteger() ? wanted : nullptr;
  this->state.walk.tasks.pushTask(FinishBitNot{.unary = unary});
  this->state.walk.tasks.pushTask(ApparentSlot{.slot = &unary->operand});

  return this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &unary->operand, .expected = hint});
}

void BitOperations::finishBitNot(const FinishBitNot &task) {
  UnaryExpr *unary = task.unary;
  const Type *type = unary->operand->type;
  auto integer = this->checkBitsOperand(type);
  if (integer.isErr()) {
    this->state.diagnostics.error(unary->loc(), integer.error().message);
  }
  unary->type = integer.isOk() ? type : Type::error();
  auto *folded = this->arithmeticRules.foldedUnary(unary);

  return this->state.walk.tasks.pushChecked(folded);
}

std::optional<CodeArithmeticRegistryCheckBitsErrorCode>
BitOperations::bitsProblem(const BinaryExpr *binary) const {
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  auto *zero = dyn_cast<IntLitExpr>(binary->rhs);
  bool isOrZero = leftType->isNumeric() && !leftType->isWrapping() && zero != nullptr &&
                  zero->value == 0 &&
                  (binary->op == BinaryOp::BitOr || binary->op == BinaryOp::UShr);
  if (isOrZero) {
    return CodeArithmeticRegistryCheckBitsErrorCode::NOTHING_CHANGES;
  }
  if (leftType->isFloat() || rightType->isFloat()) {
    return CodeArithmeticRegistryCheckBitsErrorCode::DOUBLE_OPERAND;
  }
  if (!leftType->isInteger() || !rightType->isInteger()) {
    return CodeArithmeticRegistryCheckBitsErrorCode::OPERAND_TYPES;
  }
  if (binary->op == BinaryOp::UShr && !leftType->isUnsigned()) {
    return CodeArithmeticRegistryCheckBitsErrorCode::SIGNED_SHIFT;
  }

  return std::nullopt;
}

std::string BitOperations::bitsProblemText(const BinaryExpr *binary,
                                           CodeArithmeticRegistryCheckBitsErrorCode problem) const {
  const char *op = Operators::opSpelling(binary->op);
  switch (problem) {
    case CodeArithmeticRegistryCheckBitsErrorCode::NOTHING_CHANGES:
      return this->nothingChangesText(binary);
    case CodeArithmeticRegistryCheckBitsErrorCode::DOUBLE_OPERAND:
      return this->doubleBitsText(binary);
    case CodeArithmeticRegistryCheckBitsErrorCode::OPERAND_TYPES:
      return this->arithmeticRules.operandTypesText(op, binary->lhs->type, binary->rhs->type);
    case CodeArithmeticRegistryCheckBitsErrorCode::SIGNED_SHIFT:
      return this->signedShiftText(binary);
    case CodeArithmeticRegistryCheckBitsErrorCode::MIXED_NUMBERS:
    case CodeArithmeticRegistryCheckBitsErrorCode::DIVISOR_ZERO:
      return this->arithmeticRules.mixedNumbersText(binary);
  }

  return this->arithmeticRules.mixedNumbersText(binary);
}

std::string BitOperations::nothingChangesText(const BinaryExpr *binary) const {
  const char *op = Operators::opSpelling(binary->op);
  auto [lhsText, rhsText] = SharedHelpers::operandTexts(
      binary->source, binary->loc(), std::strlen(Operators::opSpelling(binary->op)));
  llvm::StringRef lhsName = lhsText.empty() ? kValuePlaceholder : lhsText;
  auto hint = this->nothingChangesHint(binary, lhsText);

  return ErrorTextNoOpBits::nothing(lhsName, op, hint);
}

std::string BitOperations::nothingChangesHint(const BinaryExpr *binary,
                                              llvm::StringRef lhsText) const {
  const Type *leftType = binary->lhs->type;
  if (leftType->isFloat() && this->isRoundingCall(binary->lhs)) {
    return ErrorTextNoOpBits::toInt(lhsText);
  }
  if (leftType->isFloat()) {
    return ErrorTextNoOpBits::trunc(lhsText);
  }
  auto text = this->shownOperand(lhsText);
  bool isUnsignedShift = binary->op == BinaryOp::UShr;

  return ErrorTextNoOpBits::wrap(text, isUnsignedShift);
}

bool BitOperations::isRoundingCall(const Expr *expr) const {
  auto *call = dyn_cast<CallExpr>(expr);
  if (call == nullptr) {
    return false;
  }

  return BuiltinGroups::isRoundingBuiltin(call->builtin());
}

std::string BitOperations::shownOperand(llvm::StringRef text) const {
  if (text.empty()) {
    return kValuePlaceholder;
  }

  return this->state.code.conversions.operandText(text);
}

std::string BitOperations::doubleBitsText(const BinaryExpr *binary) const {
  const char *op = Operators::opSpelling(binary->op);
  auto [lhsText, rhsText] = SharedHelpers::operandTexts(
      binary->source, binary->loc(), std::strlen(Operators::opSpelling(binary->op)));
  llvm::StringRef fractionText = binary->lhs->type->isFloat() ? lhsText : rhsText;
  auto side = this->shownOperand(fractionText);

  return ErrorTextBitsDouble::fraction(op, side);
}

std::string BitOperations::signedShiftText(const BinaryExpr *binary) const {
  const Type *leftType = binary->lhs->type;
  auto [lhsText, rhsText] = SharedHelpers::operandTexts(
      binary->source, binary->loc(), std::strlen(Operators::opSpelling(binary->op)));
  auto text = this->shownOperand(lhsText);
  llvm::StringRef count = rhsText.empty() ? kCountPlaceholder : rhsText;
  const auto *unsignedType = Type::unsignedInteger(leftType->bitWidth());
  auto method = this->state.code.conversions.conversionMethod(unsignedType);

  return ErrorTextShift::isSigned(leftType->name(), text, method, count);
}

BitOperations::ResultOperandIsInteger BitOperations::checkBitsOperand(const Type *type) const {
  if (type->isFloat()) {
    auto message = ErrorTextUnary::bitsDouble();

    return err(CodeArithmeticRegistryBitsOperandError{
        .code = CodeArithmeticRegistryBitsOperandErrorCode::DOUBLE_BITS,
        .message = message,
    });
  }
  if (!type->isInteger() && !type->isError()) {
    auto message = ErrorTextUnary::bitsType(type->name());

    return err(CodeArithmeticRegistryBitsOperandError{
        .code = CodeArithmeticRegistryBitsOperandErrorCode::NOT_INTEGER,
        .message = message,
    });
  }

  return ok(type);
}

}
