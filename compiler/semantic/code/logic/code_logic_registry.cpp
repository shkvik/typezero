#include "compiler/semantic/code/logic/code_logic_registry.h"

namespace tz {

CodeLogicRegistry::CodeLogicRegistry(SemanticState &owner)
    : logicRules(owner),
      logicValues(owner, this->logicRules),
      logicBranches(owner, this->logicRules),
      nullishValues(owner) {}

void CodeLogicRegistry::checkCondition(const CheckCondition &task) {
  return this->logicBranches.checkCondition(task);
}

void CodeLogicRegistry::finishCondition() {
  return this->logicBranches.finishCondition();
}

void CodeLogicRegistry::startBranch(const StartBranch &task) {
  return this->logicBranches.startBranch(task);
}

void CodeLogicRegistry::afterBranchLeaf(const AfterBranchLeaf &task) {
  return this->logicBranches.afterBranchLeaf(task);
}

void CodeLogicRegistry::finishBranchLeaf(const FinishBranchLeaf &task) {
  return this->logicBranches.finishBranchLeaf(task);
}

void CodeLogicRegistry::afterAssignedLeaf(const AfterAssignedLeaf &task) {
  return this->logicBranches.afterAssignedLeaf(task);
}

void CodeLogicRegistry::afterBranchLeft(const AfterBranchLeft &task) {
  return this->logicBranches.afterBranchLeft(task);
}

void CodeLogicRegistry::afterBranchRight(const AfterBranchRight &task) {
  return this->logicBranches.afterBranchRight(task);
}

void CodeLogicRegistry::afterBranchNot(const AfterBranchNot &task) {
  return this->logicBranches.afterBranchNot(task);
}

void CodeLogicRegistry::checkNot(const CheckNot &task) {
  return this->logicBranches.checkNot(task);
}

void CodeLogicRegistry::checkLogicalOperator(const CheckLogicalOperator &task) {
  return this->logicValues.checkLogicalOperator(task);
}

void CodeLogicRegistry::finishBranchJoin() {
  return this->logicBranches.finishBranchJoin();
}

void CodeLogicRegistry::startTestedValue(const StartTestedValue &task) {
  return this->logicBranches.startTestedValue(task);
}

void CodeLogicRegistry::afterTestedLeaf() {
  return this->logicValues.afterTestedLeaf();
}

void CodeLogicRegistry::startLogical(const StartLogical &task) {
  return this->logicBranches.startLogical(task);
}

void CodeLogicRegistry::afterLogicalLeft(const AfterLogicalLeft &task) {
  return this->logicValues.afterLogicalLeft(task);
}

void CodeLogicRegistry::startLogicalRight(const StartLogicalRight &task) {
  return this->logicValues.startLogicalRight(task);
}

void CodeLogicRegistry::finishTestedLogical(const FinishTestedLogical &task) {
  return this->logicValues.finishTestedLogical(task);
}

void CodeLogicRegistry::finishValueLogical(const FinishValueLogical &task) {
  return this->logicValues.finishValueLogical(task);
}

void CodeLogicRegistry::finishLogicalType(const FinishLogicalType &task) {
  return this->logicValues.finishLogicalType(task);
}

void CodeLogicRegistry::startNullish(const StartNullish &task) {
  return this->nullishValues.startNullish(task);
}

void CodeLogicRegistry::afterNullishLeft(const AfterNullishLeft &task) {
  return this->nullishValues.afterNullishLeft(task);
}

void CodeLogicRegistry::startNullishRight(const StartNullishRight &task) {
  return this->nullishValues.startNullishRight(task);
}

void CodeLogicRegistry::finishNullish(const FinishNullish &task) {
  return this->nullishValues.finishNullish(task);
}

}
