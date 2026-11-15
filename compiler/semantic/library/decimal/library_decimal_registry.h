#pragma once

#include "compiler/program/library/decimal/decimal_literal/decimal_literal.h"
#include "compiler/program/library/decimal/decimal_method_table/decimal_method_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/decimal/library_decimal_registry_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include <string>
#include <utility>

namespace tz {

class LibraryDecimalRegistry {
public:
  explicit LibraryDecimalRegistry(SemanticState &owner);

  void decimalArgSlot(const DecimalArgSlot &task);
  void checkDecimalCall(const CheckDecimalCall &task);
  void reportDecimalMix(const ReportDecimalMix &task);
  void afterComparedLeft(const AfterComparedLeft &task);
  void afterRoundingMode(const AfterRoundingMode &task);
  void afterDecimalCount(const AfterDecimalCount &task);
  void walkDecimalAssign(const WalkDecimalAssign &task);
  void finishDecimalText(const FinishDecimalText &task);
  void afterComparedRight(const AfterComparedRight &task);
  void convertDecimalSide(const ConvertDecimalSide &task);
  void afterDecimalAssign(const AfterDecimalAssign &task);
  void checkDecimalStatic(const CheckDecimalStatic &task);
  void afterDecimalSpread(const AfterDecimalSpread &task);
  void checkDecimalMethod(const CheckDecimalMethod &task);
  void afterDecimalCallArg(const AfterDecimalCallArg &task);
  void finishDecimalStatic(const FinishDecimalStatic &task);
  void finishDecimalMethod(const FinishDecimalMethod &task);
  void checkDecimalArithmetic(const CheckDecimalArithmetic &task);
  void checkDecimalComparison(const CheckDecimalComparison &task);
  void walkComparisonOperands(const WalkComparisonOperands &task);
  void finishDecimalArithmetic(const FinishDecimalArithmetic &task);
  void checkDecimalLiteralRange(const CheckDecimalLiteralRange &task);

private:
  using ResultRoundingMode = Result<int, LibraryDecimalRegistryRoundingModeError>;
  using ResultLiteralRange = Result<DecimalLiteral, LibraryDecimalRegistryLiteralRangeError>;

  void checkDecimalText(CallExpr *call);
  void reportMix(const BinaryExpr *binary);
  void checkDecimalPower(BinaryExpr *binary);
  void walkDecimalDivisor(BinaryOp op, const Expr *divisor);
  void walkKnownModes(llvm::ArrayRef<std::string> modes, const Expr *arg);
  void pushDecimalCount(Expr **slot, std::string what, int64_t low, int64_t high);


  bool isLiteralTree(const Expr *expr) const;
  bool isLiteralNode(const Expr *expr) const;
  bool isFittingSpread(const Expr *arg) const;
  bool isArithmeticOperator(BinaryOp op) const;
  bool isDecimalText(const Type *type) const;
  bool isComparedLiteral(const Expr *expr) const;
  bool isStringLiteralType(const Type *type) const;
  bool isDecimalNumberText(const std::string &text) const;
  bool isFittingLiteral(const Expr *expr, const Type *type) const;
  bool reportBadDecimalText(llvm::ArrayRef<std::string> texts, const Expr *arg);


  const Type *decimalResultType(DecimalResult result) const;
  const Type *literalHint(const Expr *other, const Type *hint) const;
  const Type *comparisonHint(const Expr *other, const Expr *self) const;


  ResultLiteralRange checkLiteralRange(const Expr *literal) const;
  std::pair<llvm::StringRef, llvm::StringRef> sidesOf(const BinaryExpr *binary) const;
  ResultRoundingMode checkRoundingModeText(const std::u16string &text) const;
  std::string operandTypesText(llvm::StringRef op, const Type *lhs, const Type *rhs) const;

  SemanticState &state;
};

}
