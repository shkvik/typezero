#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// new: which class checks the construction.
class NewChecker {
public:
  explicit NewChecker(Checker &checker);

  Expr *checkNew(NewExpr *expr, const Type *expected);

private:
  Checker &checker_;
};

} // namespace tz
