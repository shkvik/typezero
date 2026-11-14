#pragma once

#include "compiler/semantic/code/comparisons/code_comparisons_registry_errors.h"
#include "compiler/semantic/code/comparisons/comparison_rules/comparison_rules.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class OrderComparisons {
public:
  OrderComparisons(SemanticState &owner, ComparisonRules &sharedComparisonRules);

  void checkComparison(const CheckComparison &task);
  void compareOperands(const CompareOperands &task);
  void finishComparison(const FinishComparison &task);
  void finishNumberComparison(const FinishNumberComparison &task);
  void afterComparisonOperands(const AfterComparisonOperands &task);

private:
  SemanticState &state;
  ComparisonRules &comparisonRules;
};

}
