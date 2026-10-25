#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Unary operators, delete and in.
class UnaryChecker {
public:
  explicit UnaryChecker(Checker &checker);

  Expr *checkUnary(UnaryExpr *unary, const Type *expected, bool wraps);
  // `delete x[i]` and `key in x`: errors but for dictionaries (see sema.cpp).
  Expr *checkDelete(UnaryExpr *unary);
  Expr *checkIn(BinaryExpr *bin);

private:
  Checker &checker_;
};

} // namespace tz
