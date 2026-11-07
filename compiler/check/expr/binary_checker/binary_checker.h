#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class BinaryChecker {
public:
  explicit BinaryChecker(Checker &owner);

  Expr *checkBinary(BinaryExpr *binary, const Type *expected, bool wraps);
  Expr *checkEquality(BinaryExpr *bin);
  Expr *checkNullish(BinaryExpr *bin, const Type *expected);
  Expr *toInt32(Expr *expr);
  void reportOperandTypes(llvm::SMLoc loc, const char *op, const Type *lhs, const Type *rhs);
  void reportMixedNumbers(const BinaryExpr *bin);
  void adaptLiterals(BinaryExpr *bin);
  Expr *checkBits(BinaryExpr *bin);
  Expr *checkWrapping(BinaryExpr *bin);
  bool checkTypeOfName(const Expr *operand, const StringLitExpr *text);

private:
  static bool onlyIntLiterals(const Expr *expr);
  static const Type *integerHint(const Expr *literal, const Expr *other, const Type *hint,
                                 bool bitwise);

  Checker &checker;
};

}
