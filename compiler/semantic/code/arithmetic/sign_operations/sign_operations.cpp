#include "compiler/semantic/code/arithmetic/sign_operations/sign_operations.h"
#include "compiler/semantic/code/operators/code_operators_registry_errors.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/CheckedArithmetic.h"
#include <cstdio>

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

SignOperations::SignOperations(SemanticState &owner, ArithmeticRules &sharedArithmeticRules)
    : state(owner),
      arithmeticRules(sharedArithmeticRules) {}

void SignOperations::checkSign(const CheckSign &task) {
  UnaryExpr *unary = task.unary;
  auto *integer = dyn_cast<IntLitExpr>(unary->operand);
  auto *real = dyn_cast<FloatLitExpr>(unary->operand);
  bool isNegated = unary->op == UnaryOp::Neg;
  const auto &literals = this->state.types.literals;
  if (isNegated && integer != nullptr) {
    auto value = -double(integer->value);
    const auto *literal = literals.numberLiteralFor(task.expected, value, NumberSpelling::WHOLE);
    if (literal != nullptr && integer->value != 0) {
      unary->type = literal;
      this->state.walk.tasks.pushChecked(unary);
      this->state.walk.tasks.pushTask(WriteCheckedSlot{.slot = &unary->operand});
      return this->state.walk.tasks.pushTask(CheckIntLiteral{
          .literal = integer,
          .expected = literal->literalBase(),
          .sign = LiteralSign::NEGATED,
      });
    }
  }
  if (isNegated && real != nullptr) {
    const auto *literal =
        literals.numberLiteralFor(task.expected, -real->value, NumberSpelling::FRACTION);
    if (literal != nullptr) {
      real->type = Type::doubleTy();
      unary->type = literal;
      return this->state.walk.tasks.pushChecked(unary);
    }
  }
  const auto *hint = this->signHint(unary, task.expected);
  this->state.walk.tasks.pushTask(FinishSign{.unary = unary});
  if (integer != nullptr) {
    auto sign = isNegated ? LiteralSign::NEGATED : LiteralSign::PLAIN;
    this->state.walk.tasks.pushTask(WriteCheckedSlot{.slot = &unary->operand});
    return this->state.walk.tasks.pushTask(CheckIntLiteral{
        .literal = integer,
        .expected = hint,
        .sign = sign,
    });
  }
  this->state.walk.tasks.pushTask(ApparentSlot{.slot = &unary->operand});

  return this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &unary->operand, .expected = hint});
}

void SignOperations::finishSign(const FinishSign &task) {
  UnaryExpr *unary = task.unary;
  if (!unary->operand->type->isNever()) {
    auto sign = SignOperandReady{.unary = unary};
    return this->signOperandReady(sign);
  }
  this->state.walk.tasks.pushTask(SignOperandReady{.unary = unary});

  return this->state.walk.tasks.pushTask(
      ConvertSlot{.slot = &unary->operand, .to = Type::doubleTy()});
}

void SignOperations::signOperandReady(const SignOperandReady &task) {
  UnaryExpr *unary = task.unary;
  const Type *type = unary->operand->type;
  const char *op = Operators::opSpelling(unary->op);
  auto sign = this->checkSignOperand(op, type);
  if (sign.isErr()) {
    this->state.diagnostics.error(unary->loc(), sign.error().message);
    unary->type = Type::error();
    auto *folded = this->arithmeticRules.foldedUnary(unary);
    return this->state.walk.tasks.pushChecked(folded);
  }
  if (type->isDate() && unary->op == UnaryOp::Plus) {
    this->state.walk.tasks.pushChecked(unary->operand);
    return this->state.walk.tasks.pushTask(DateToTime{});
  }
  bool isLiteral = isa<IntLitExpr>(unary->operand) || isa<FloatLitExpr>(unary->operand);
  auto numeric = this->checkNumberOperand(unary, op, type, isLiteral);
  if (numeric.isErr()) {
    this->state.diagnostics.error(unary->loc(), numeric.error().message);
  }
  unary->type = numeric.isOk() ? type : Type::error();
  auto *folded = this->arithmeticRules.foldedUnary(unary);

  return this->state.walk.tasks.pushChecked(folded);
}

SignOperations::ResultNumberForTheSign SignOperations::checkSignOperand(llvm::StringRef op,
                                                                        const Type *type) const {
  if (type->isString() || type->isBool()) {
    auto message = ErrorTextUnary::noNumber(op, type->isString());

    return err(CodeArithmeticRegistrySignOperandError{
        .code = CodeArithmeticRegistrySignOperandErrorCode::STRING_OR_BOOL,
        .message = message,
    });
  }

  return ok(type);
}

SignOperations::ResultOperandIsNumeric SignOperations::checkNumberOperand(const UnaryExpr *unary,
                                                                          llvm::StringRef op,
                                                                          const Type *type,
                                                                          bool isLiteral) const {
  if (type->isDecimal() && unary->op == UnaryOp::Plus && !isLiteral) {
    auto message = ErrorTextUnary::decimalSum();

    return err(CodeArithmeticRegistryNumberOperandError{
        .code = CodeArithmeticRegistryNumberOperandErrorCode::DECIMAL_PLUS,
        .message = message,
    });
  }
  if (!type->isNumeric() && !type->isDecimal() && !type->isError()) {
    auto message = ErrorTextUnary::notNumber(op, type->name());

    return err(CodeArithmeticRegistryNumberOperandError{
        .code = CodeArithmeticRegistryNumberOperandErrorCode::NOT_NUMERIC,
        .message = message,
    });
  }

  return ok(type);
}

const Type *SignOperations::signHint(const UnaryExpr *unary, const Type *expected) const {
  bool isFraction = isa<FloatLitExpr>(unary->operand);
  auto spelling = isFraction ? NumberSpelling::FRACTION : NumberSpelling::WHOLE;
  const auto *member = this->state.types.literals.numberMemberOf(expected, spelling);
  const Type *plain = expected != nullptr ? expected->nonOptional() : nullptr;
  const Type *wanted = member != nullptr ? member : plain;
  bool isNumeric = wanted != nullptr && (wanted->isNumeric() || wanted->isDecimal());

  return isNumeric ? wanted : nullptr;
}

}
