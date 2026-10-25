#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Unary operators, delete and in.
class UnaryChecker {
public:
  explicit UnaryChecker(Sema &sema);

  Expr *checkUnary(UnaryExpr *unary, const Type *expected, bool wraps);
  // `delete x[i]` and `key in x`: errors but for dictionaries (see sema.cpp).
  Expr *checkDelete(UnaryExpr *unary);
  Expr *checkIn(BinaryExpr *bin);

private:
  Sema &sema_;
};

} // namespace tz
