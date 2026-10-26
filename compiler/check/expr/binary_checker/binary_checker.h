#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class BinaryChecker {
public:
  explicit BinaryChecker(Checker &checker);

  Expr *checkBinary(BinaryExpr *binary, const Type *expected, bool wraps);
  Expr *checkEquality(BinaryExpr *bin);
  Expr *checkNullish(BinaryExpr *bin, const Type *expected);
  Expr *toInt32(Expr *expr);
  void reportOperandTypes(llvm::SMLoc loc, const char *op, const Type *lhs, const Type *rhs);

private:
  Checker &checker_;
};

}
