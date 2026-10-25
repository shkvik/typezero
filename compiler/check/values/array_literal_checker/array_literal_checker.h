#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Array literals and their spreads.
class ArrayLiteralChecker {
public:
  explicit ArrayLiteralChecker(Checker &checker);

  Expr *checkArrayLit(ArrayLitExpr *lit, const Type *expected);

private:
  Checker &checker_;
};

} // namespace tz
