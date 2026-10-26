#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ArrayLiteralChecker {
public:
  explicit ArrayLiteralChecker(Checker &checker);

  Expr *checkArrayLit(ArrayLitExpr *lit, const Type *expected);

private:
  Checker &checker_;
};

}
