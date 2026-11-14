#pragma once

#include "compiler/semantic/code/comparisons/comparison_rules/comparison_rules.h"
#include "compiler/semantic/code/comparisons/equality_checks/equality_checks.h"
#include "compiler/semantic/code/comparisons/in_operators/in_operators.h"
#include "compiler/semantic/code/comparisons/order_comparisons/order_comparisons.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class CodeComparisonsRegistry {
public:
  explicit CodeComparisonsRegistry(SemanticState &owner);

  void checkEquality(const CheckEquality &task);
  void walkTypeOfName(const WalkTypeOfName &task);
  void checkComparison(const CheckComparison &task);
  void compareOperands(const CompareOperands &task);
  void checkInOperator(const CheckInOperator &task);
  void afterInOperands(const AfterInOperands &task);
  void finishComparison(const FinishComparison &task);
  void finishEqualOperands(const FinishEqualOperands &task);
  void afterEqualityOperands(const AfterEqualityOperands &task);
  void finishNumberComparison(const FinishNumberComparison &task);
  void afterComparisonOperands(const AfterComparisonOperands &task);

private:
  ComparisonRules comparisonRules;
  InOperators inOperators;
  EqualityChecks equalityChecks;
  OrderComparisons orderComparisons;
};

}
