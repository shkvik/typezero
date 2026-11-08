#pragma once

#include "compiler/program/library/decimal/decimal_literal/decimal_literal.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class DecimalChecker {
public:
  explicit DecimalChecker(SemanticState &owner);

  static bool isLiteralTree(const Expr *expr);
  static std::pair<llvm::StringRef, llvm::StringRef> sidesOf(const BinaryExpr *bin);
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
  Expr *checkDecimalCount(Expr *arg, llvm::StringRef what, int64_t lo, int64_t hi);

private:
  using ResultRoundingMode = Result<int, DecimalCheckerRoundingModeError>;
  using ResultLiteralRange = Result<DecimalLiteral, DecimalCheckerLiteralRangeError>;

  static ResultLiteralRange literalRange(const Expr *lit);
  static ResultRoundingMode roundingMode(const std::u16string &text);

  SemanticState &state;
};

}
