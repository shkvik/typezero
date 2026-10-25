#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Array literals and their spreads.
class ArrayLiteralChecker {
public:
  explicit ArrayLiteralChecker(Sema &sema);

  Expr *checkArrayLit(ArrayLitExpr *lit, const Type *expected);

private:
  Sema &sema_;
};

} // namespace tz
