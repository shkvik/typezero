#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/expressions/code_expressions_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <vector>

namespace tz {

class ConditionalExpressions {
public:
  explicit ConditionalExpressions(SemanticState &owner);

  void afterElseFirst(const AfterElseFirst &task);
  void afterThenFirst(const AfterThenFirst &task);
  void afterThenSecond(const AfterThenSecond &task);
  void afterElseSecond(const AfterElseSecond &task);
  void finishConditional(const FinishConditional &task);
  void afterConditionTest(const AfterConditionTest &task);
  void joinConditionalBranches(const JoinConditionalBranches &task);

private:
  using ResultBranchesJoined = Result<const Type *, CodeExpressionsRegistryCheckConditionalError>;

  bool isLiteralShape(const Expr *expr) const;
  bool isLiteralShaped(const Expr *expr) const;
  bool isWaitingForOther(const Expr *branch) const;
  bool isResultLiteral(const CallExpr *call) const;


  const Type *failedBranchesType(const Type *thenType, const Type *elseType) const;
  const Type *branchHint(const Expr *literal, const Expr *other, const Type *expected) const;
  const Type *elseHint(const ConditionalExpr *conditional, const Type *expected,
                       bool isElseWaitingOnly) const;


  std::vector<const Expr *> shapeChoices(const Expr *expr) const;
  std::vector<const Expr *> mappedReturns(const CallExpr *call) const;
  std::vector<const Expr *> findReturnValues(const BlockStmt *body) const;


  ResultBranchesJoined checkBranchesJoin(const Type *left, const Type *right) const;

  SemanticState &state;
};

}
