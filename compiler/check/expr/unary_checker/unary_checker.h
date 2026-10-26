#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class UnaryChecker {
public:
  explicit UnaryChecker(Checker &checker);

  Expr *checkUnary(UnaryExpr *unary, const Type *expected, bool wraps);
  Expr *checkDelete(UnaryExpr *unary);
  Expr *checkIn(BinaryExpr *bin);

private:
  Checker &checker_;
};

}
