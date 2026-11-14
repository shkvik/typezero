#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/logic/code_logic_registry_errors.h"
#include "compiler/semantic/code/logic/logic_rules/logic_rules.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>
#include <vector>

namespace tz {

class LogicBranches {
public:
  LogicBranches(SemanticState &owner, LogicRules &sharedLogicRules);

  void finishCondition();
  void finishBranchJoin();
  void checkNot(const CheckNot &task);
  void startBranch(const StartBranch &task);
  void startLogical(const StartLogical &task);
  void checkCondition(const CheckCondition &task);
  void afterBranchNot(const AfterBranchNot &task);
  void afterBranchLeaf(const AfterBranchLeaf &task);
  void afterBranchLeft(const AfterBranchLeft &task);
  void finishBranchLeaf(const FinishBranchLeaf &task);
  void afterBranchRight(const AfterBranchRight &task);
  void startTestedValue(const StartTestedValue &task);
  void afterAssignedLeaf(const AfterAssignedLeaf &task);

private:
  using ResultConditionForm = Result<const AssignExpr *, CodeLogicRegistryAssignmentConditionError>;

  bool isLiteral(const Expr *expr) const;
  bool isBooleanCall(const Expr *expr) const;
  bool isBoolPass(const CastExpr *conversion) const;
  bool isTypeOfTest(const Expr *side, const Expr *other) const;


  std::optional<bool> instanceConstant(const Expr *cond) const;
  std::optional<bool> isJsonCondition(const CallExpr *call) const;
  std::optional<bool> typeOfCondition(const BinaryExpr *binary) const;
  std::optional<bool> undefinedCondition(const BinaryExpr *binary) const;
  ResultConditionForm checkAssignmentCondition(const AssignExpr *assign, TruthUse use) const;
  std::optional<bool> typeOfSide(const BinaryExpr *binary, const Expr *side,
                                 const Expr *other) const;
  std::optional<bool> nodeCondition(const Expr *cond,
                                    std::vector<std::optional<bool>> &answers) const;
  std::optional<bool> undefinedSide(const BinaryExpr *binary, const Expr *side,
                                    const Expr *other) const;
  std::optional<bool> typeOfAnswer(const BinaryExpr *binary, const Type *type,
                                   const StringLitExpr *text) const;
  std::optional<bool> logicalCondition(const BinaryExpr *binary,
                                       std::vector<std::optional<bool>> &answers) const;


  NodeChildren<const Expr *> conditionParts(const Expr *cond) const;

  SemanticState &state;
  LogicRules &logicRules;
};

}
