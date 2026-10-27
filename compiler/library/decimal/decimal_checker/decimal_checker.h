#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class DecimalChecker {
public:
  explicit DecimalChecker(Checker &checker);

  static bool isLiteralTree(const Expr *expr);
  static const Type *literalHint(const Expr *other, const Type *hint);
  static bool fitsLiteral(const Expr *expr, const Type *type);
  void checkComparisonOperands(BinaryExpr *bin);
  static std::optional<DecimalLiteral> decimalLiteral(const Expr *expr);
  void checkDecimalLiteralRange(const Expr *lit);
  Expr *checkDecimalArithmetic(BinaryExpr *bin);
  Expr *checkDecimalComparison(BinaryExpr *bin);
  void reportDecimalMix(const BinaryExpr *bin);
  void checkDecimalAssign(AssignExpr *assign, BinaryOp op);
  void checkDecimalDivisor(BinaryOp op, const Expr *divisor);
  Expr *checkDecimalCall(CallExpr *call, llvm::SMLoc loc);
  Expr *checkDecimalStatic(CallExpr *call, MemberExpr *member);
  Expr *checkDecimalMethod(CallExpr *call, MemberExpr *member);
  Expr *checkRoundingMode(Expr *arg);
  Expr *checkDecimalCount(Expr *arg, const char *what, int64_t lo, int64_t hi);
  void reportBigInt(llvm::SMLoc loc, llvm::StringRef what);

private:
  Checker &checker_;
};

}
