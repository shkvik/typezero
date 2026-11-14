#include "compiler/semantic/code/comparisons/code_comparisons_registry.h"

namespace tz {

CodeComparisonsRegistry::CodeComparisonsRegistry(SemanticState &owner)
    : comparisonRules(owner),
      inOperators(owner, this->comparisonRules),
      equalityChecks(owner, this->comparisonRules),
      orderComparisons(owner, this->comparisonRules) {}

void CodeComparisonsRegistry::checkComparison(const CheckComparison &task) {
  return this->orderComparisons.checkComparison(task);
}

void CodeComparisonsRegistry::afterComparisonOperands(const AfterComparisonOperands &task) {
  return this->orderComparisons.afterComparisonOperands(task);
}

void CodeComparisonsRegistry::finishComparison(const FinishComparison &task) {
  return this->orderComparisons.finishComparison(task);
}

void CodeComparisonsRegistry::compareOperands(const CompareOperands &task) {
  return this->orderComparisons.compareOperands(task);
}

void CodeComparisonsRegistry::finishNumberComparison(const FinishNumberComparison &task) {
  return this->orderComparisons.finishNumberComparison(task);
}

void CodeComparisonsRegistry::checkEquality(const CheckEquality &task) {
  return this->equalityChecks.checkEquality(task);
}

void CodeComparisonsRegistry::afterEqualityOperands(const AfterEqualityOperands &task) {
  return this->equalityChecks.afterEqualityOperands(task);
}

void CodeComparisonsRegistry::finishEqualOperands(const FinishEqualOperands &task) {
  return this->equalityChecks.finishEqualOperands(task);
}

void CodeComparisonsRegistry::walkTypeOfName(const WalkTypeOfName &task) {
  return this->equalityChecks.walkTypeOfName(task);
}

void CodeComparisonsRegistry::checkInOperator(const CheckInOperator &task) {
  return this->inOperators.checkInOperator(task);
}

void CodeComparisonsRegistry::afterInOperands(const AfterInOperands &task) {
  return this->inOperators.afterInOperands(task);
}

}
