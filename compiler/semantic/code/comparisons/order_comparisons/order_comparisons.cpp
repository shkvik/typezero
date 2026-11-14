#include "compiler/semantic/code/comparisons/order_comparisons/order_comparisons.h"

namespace tz {

OrderComparisons::OrderComparisons(SemanticState &owner, ComparisonRules &sharedComparisonRules)
    : state(owner),
      comparisonRules(sharedComparisonRules) {}

void OrderComparisons::checkComparison(const CheckComparison &task) {
  this->state.walk.tasks.pushTask(AfterComparisonOperands{.binary = task.binary});

  return this->state.walk.tasks.pushTask(WalkComparisonOperands{.binary = task.binary});
}

void OrderComparisons::afterComparisonOperands(const AfterComparisonOperands &task) {
  this->state.walk.tasks.pushTask(FinishComparison{.binary = task.binary});

  return this->state.walk.tasks.pushTask(NeverOperands{.binary = task.binary});
}

void OrderComparisons::finishComparison(const FinishComparison &task) {
  BinaryExpr *binary = task.binary;
  binary->type = Type::boolTy();
  bool isSameParam = binary->lhs->type->isParam() && binary->lhs->type == binary->rhs->type;
  if (isSameParam) {
    return this->state.walk.tasks.pushChecked(binary);
  }
  const auto &apparent = this->state.generics.apparent;
  const Type *leftType = apparent.apparentType(binary->lhs->type);
  const Type *rightType = apparent.apparentType(binary->rhs->type);
  this->state.walk.tasks.pushTask(CompareOperands{.binary = binary});
  if (rightType != binary->rhs->type) {
    this->state.walk.tasks.pushTask(ConvertSlot{.slot = &binary->rhs, .to = rightType});
  }
  if (leftType != binary->lhs->type) {
    this->state.walk.tasks.pushTask(ConvertSlot{.slot = &binary->lhs, .to = leftType});
  }
}

void OrderComparisons::compareOperands(const CompareOperands &task) {
  BinaryExpr *binary = task.binary;
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  if (leftType->isError() || rightType->isError()) {
    return this->state.walk.tasks.pushChecked(binary);
  }
  if (leftType->isString() && rightType->isString()) {
    return this->state.walk.tasks.pushChecked(binary);
  }
  if (leftType->isDate() || rightType->isDate()) {
    return this->state.walk.tasks.pushTask(CheckDateOperands{.binary = binary});
  }
  if (leftType->isDecimal() || rightType->isDecimal()) {
    return this->state.walk.tasks.pushTask(CheckDecimalComparison{.binary = binary});
  }
  if (leftType->isNumeric() && rightType->isNumeric()) {
    this->state.walk.tasks.pushTask(FinishNumberComparison{.binary = binary});
    return this->comparisonRules.pushLiteralAdapting(binary);
  }
  const char *op = Operators::opSpelling(binary->op);
  auto message = this->comparisonRules.operandTypesText(op, leftType, rightType);
  this->state.diagnostics.error(binary->loc(), message);

  return this->state.walk.tasks.pushChecked(binary);
}

void OrderComparisons::finishNumberComparison(const FinishNumberComparison &task) {
  BinaryExpr *binary = task.binary;
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  bool isComparable = leftType == rightType || (leftType->isInteger() && rightType->isInteger());
  if (!isComparable) {
    auto message = this->comparisonRules.mixedNumbersText(binary);
    this->state.diagnostics.error(binary->loc(), message);
  }

  return this->state.walk.tasks.pushChecked(binary);
}

}
