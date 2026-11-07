#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class NewChecker {
public:
  explicit NewChecker(Checker &owner);

  Expr *checkNew(NewExpr *expr, const Type *expected);

private:
  Checker &checker;
};

}
