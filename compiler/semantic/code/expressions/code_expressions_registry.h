#pragma once

#include "compiler/semantic/code/expressions/cast_expressions/cast_expressions.h"
#include "compiler/semantic/code/expressions/conditional_expressions/conditional_expressions.h"
#include "compiler/semantic/code/expressions/expression_dispatch/expression_dispatch.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class CodeExpressionsRegistry {
public:
  explicit CodeExpressionsRegistry(SemanticState &owner);

  void checkExpr(const CheckExpr &task);
  void finishCast(const FinishCast &task);
  void reportError(const ReportError &task);
  void checkExprSlot(const CheckExprSlot &task);
  void checkExprKind(const CheckExprKind &task);
  void finishSequence(const FinishSequence &task);
  void afterElseFirst(const AfterElseFirst &task);
  void afterThenFirst(const AfterThenFirst &task);
  void afterThenSecond(const AfterThenSecond &task);
  void afterElseSecond(const AfterElseSecond &task);
  void afterCastTarget(const AfterCastTarget &task);
  void writeCheckedSlot(const WriteCheckedSlot &task);
  void nextSequenceItem(const NextSequenceItem &task);
  void afterSequenceItem(const AfterSequenceItem &task);
  void finishConditional(const FinishConditional &task);
  void afterConditionTest(const AfterConditionTest &task);
  void finishOptionalChain(const FinishOptionalChain &task);
  void joinConditionalBranches(const JoinConditionalBranches &task);

private:
  CastExpressions castExpressions;
  ExpressionDispatch expressionDispatch;
  ConditionalExpressions conditionalExpressions;
};

}
