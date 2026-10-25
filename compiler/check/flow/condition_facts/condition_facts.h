#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The facts a condition gives its branches.
class ConditionFacts {
public:
  explicit ConditionFacts(Sema &sema);

  void addConditionFacts(const Expr *cond, Sema::Flow &whenTrue, Sema::Flow &whenFalse) const;

private:
  Sema &sema_;
};

} // namespace tz
