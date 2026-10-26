#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ConditionFacts {
public:
  explicit ConditionFacts(Checker &checker);

  void addConditionFacts(const Expr *cond, Checker::Flow &whenTrue, Checker::Flow &whenFalse) const;

private:
  Checker &checker_;
};

}
