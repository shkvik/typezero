#pragma once

#include "compiler/semantic/code/logic/logic_branches/logic_branches.h"
#include "compiler/semantic/code/logic/logic_rules/logic_rules.h"
#include "compiler/semantic/code/logic/logic_values/logic_values.h"
#include "compiler/semantic/code/logic/nullish_values/nullish_values.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class CodeLogicRegistry {
public:
  explicit CodeLogicRegistry(SemanticState &owner);

  void finishCondition();
  void afterTestedLeaf();
  void finishBranchJoin();
  void checkNot(const CheckNot &task);
  void startBranch(const StartBranch &task);
  void startLogical(const StartLogical &task);
  void startNullish(const StartNullish &task);
  void finishNullish(const FinishNullish &task);
  void checkCondition(const CheckCondition &task);
  void afterBranchNot(const AfterBranchNot &task);
  void afterBranchLeaf(const AfterBranchLeaf &task);
  void afterBranchLeft(const AfterBranchLeft &task);
  void finishBranchLeaf(const FinishBranchLeaf &task);
  void afterBranchRight(const AfterBranchRight &task);
  void startTestedValue(const StartTestedValue &task);
  void afterLogicalLeft(const AfterLogicalLeft &task);
  void afterNullishLeft(const AfterNullishLeft &task);
  void afterAssignedLeaf(const AfterAssignedLeaf &task);
  void startLogicalRight(const StartLogicalRight &task);
  void finishLogicalType(const FinishLogicalType &task);
  void startNullishRight(const StartNullishRight &task);
  void finishValueLogical(const FinishValueLogical &task);
  void finishTestedLogical(const FinishTestedLogical &task);
  void checkLogicalOperator(const CheckLogicalOperator &task);

private:
  LogicRules logicRules;
  LogicValues logicValues;
  LogicBranches logicBranches;
  NullishValues nullishValues;
};

}
