#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Binary operators, equality and ??.
class BinaryChecker {
public:
  explicit BinaryChecker(Sema &sema);

  Expr *checkBinary(BinaryExpr *binary, const Type *expected, bool wraps);
  Expr *checkEquality(BinaryExpr *bin);
  Expr *checkNullish(BinaryExpr *bin, const Type *expected);
  // An operand of a bitwise operator as an int, the way JavaScript's ToInt32
  // makes one: a double loses its fraction and wraps around, NaN and the
  // infinities become 0; a long keeps its low 32 bits.
  Expr *toInt32(Expr *expr);
  void reportOperandTypes(llvm::SMLoc loc, const char *op, const Type *lhs, const Type *rhs);

private:
  Sema &sema_;
};

} // namespace tz
