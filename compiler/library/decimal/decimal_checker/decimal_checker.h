#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// decimal: literals, operators and methods (docs/decimal.md).
class DecimalChecker {
public:
  explicit DecimalChecker(Sema &sema);

  // decimal (sema_decimal.cpp). A number literal, or an expression made only
  // of literals, read where a decimal is expected or next to a decimal
  // operand takes its exact value from the text of the program. Literals have
  // no effects, so checking the other operand first changes nothing else.
  static bool isLiteralTree(const Expr *expr);
  // The hint for a literal tree next to `other`, which is checked: decimal if
  // `other` is one.
  static const Type *literalHint(const Expr *other, const Type *hint);
  // Checks both operands of a comparison, in that order.
  void checkComparisonOperands(BinaryExpr *bin);
  // The exact value of a checked number literal or its negation, if it is one.
  static std::optional<DecimalLiteral> decimalLiteral(const Expr *expr);
  void checkDecimalLiteralRange(const Expr *lit);
  // Arithmetic and comparisons where a side is a decimal, both sides checked:
  // an int or a long becomes a decimal, a double is an error.
  Expr *checkDecimalArithmetic(BinaryExpr *bin);
  Expr *checkDecimalComparison(BinaryExpr *bin);
  // Reports the sides of `bin`, one a decimal, which do not go together.
  void reportDecimalMix(const BinaryExpr *bin);
  // `x op= v` on a decimal x, with v checked.
  void checkDecimalAssign(AssignExpr *assign, BinaryOp op);
  // A decimal divided by a constant zero.
  void checkDecimalDivisor(BinaryOp op, const Expr *divisor);
  Expr *checkDecimalCall(CallExpr *call, llvm::SMLoc loc);
  Expr *checkDecimalStatic(CallExpr *call, MemberExpr *member);
  Expr *checkDecimalMethod(CallExpr *call, MemberExpr *member);
  // A rounding mode argument: a string, checked now if it is a literal.
  Expr *checkRoundingMode(Expr *arg);
  // An integer argument of a decimal method, computed as long.
  Expr *checkDecimalCount(Expr *arg, const char *what);
  // `BigInt(x)`, the type bigint and the like, which do not exist.
  void reportBigInt(llvm::SMLoc loc, llvm::StringRef what);

private:
  Sema &sema_;
};

} // namespace tz
