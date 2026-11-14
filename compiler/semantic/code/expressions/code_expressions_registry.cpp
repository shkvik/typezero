#include "compiler/semantic/code/expressions/code_expressions_registry.h"

namespace tz {

CodeExpressionsRegistry::CodeExpressionsRegistry(SemanticState &owner)
    : castExpressions(owner),
      expressionDispatch(owner),
      conditionalExpressions(owner) {}

void CodeExpressionsRegistry::checkExpr(const CheckExpr &task) {
  return this->expressionDispatch.checkExpr(task);
}

void CodeExpressionsRegistry::checkExprSlot(const CheckExprSlot &task) {
  return this->expressionDispatch.checkExprSlot(task);
}

void CodeExpressionsRegistry::writeCheckedSlot(const WriteCheckedSlot &task) {
  return this->expressionDispatch.writeCheckedSlot(task);
}

void CodeExpressionsRegistry::reportError(const ReportError &task) {
  return this->expressionDispatch.reportError(task);
}

void CodeExpressionsRegistry::checkExprKind(const CheckExprKind &task) {
  return this->expressionDispatch.checkExprKind(task);
}

void CodeExpressionsRegistry::nextSequenceItem(const NextSequenceItem &task) {
  return this->expressionDispatch.nextSequenceItem(task);
}

void CodeExpressionsRegistry::afterSequenceItem(const AfterSequenceItem &task) {
  return this->expressionDispatch.afterSequenceItem(task);
}

void CodeExpressionsRegistry::finishSequence(const FinishSequence &task) {
  return this->expressionDispatch.finishSequence(task);
}

void CodeExpressionsRegistry::finishOptionalChain(const FinishOptionalChain &task) {
  return this->expressionDispatch.finishOptionalChain(task);
}

void CodeExpressionsRegistry::afterConditionTest(const AfterConditionTest &task) {
  return this->conditionalExpressions.afterConditionTest(task);
}

void CodeExpressionsRegistry::afterElseFirst(const AfterElseFirst &task) {
  return this->conditionalExpressions.afterElseFirst(task);
}

void CodeExpressionsRegistry::afterThenSecond(const AfterThenSecond &task) {
  return this->conditionalExpressions.afterThenSecond(task);
}

void CodeExpressionsRegistry::afterThenFirst(const AfterThenFirst &task) {
  return this->conditionalExpressions.afterThenFirst(task);
}

void CodeExpressionsRegistry::afterElseSecond(const AfterElseSecond &task) {
  return this->conditionalExpressions.afterElseSecond(task);
}

void CodeExpressionsRegistry::finishConditional(const FinishConditional &task) {
  return this->conditionalExpressions.finishConditional(task);
}

void CodeExpressionsRegistry::joinConditionalBranches(const JoinConditionalBranches &task) {
  return this->conditionalExpressions.joinConditionalBranches(task);
}

void CodeExpressionsRegistry::afterCastTarget(const AfterCastTarget &task) {
  return this->castExpressions.afterCastTarget(task);
}

void CodeExpressionsRegistry::finishCast(const FinishCast &task) {
  return this->castExpressions.finishCast(task);
}

}
