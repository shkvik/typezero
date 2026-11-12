#pragma once

#include "compiler/program/library/decimal/decimal_literal/decimal_literal.h"
#include "compiler/program/library/decimal/decimal_method_table/decimal_method_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class Prelude;
class ExprChecker;
class IndexProofs;
class LiteralTypes;
class TypeRelations;
class BinaryChecker;
class ConstantFolder;
class ArgumentChecker;

class DecimalChecker {
public:
  DecimalChecker(SemanticState &owner, SemanticParts &all);

  bool isLiteralTree(const Expr *expr) const;
  std::pair<llvm::StringRef, llvm::StringRef> sidesOf(const BinaryExpr *binary) const;
  const Type *literalHint(const Expr *other, const Type *hint) const;
  bool isFittingLiteral(const Expr *expr, const Type *type) const;
  void walkComparisonOperands(BinaryExpr *binary);
  using ResultDecimalLiteralChecked = Result<void, DecimalCheckerLiteralRangeError>;

  ResultDecimalLiteralChecked checkDecimalLiteralRange(const Expr *literal);
  Expr *checkDecimalArithmetic(BinaryExpr *binary);
  Expr *checkDecimalComparison(BinaryExpr *binary);
  void reportDecimalMix(const BinaryExpr *binary);
  void walkDecimalAssign(AssignExpr *assign, BinaryOp op);
  void walkDecimalDivisor(BinaryOp op, const Expr *divisor);
  Expr *checkDecimalCall(CallExpr *call, llvm::SMLoc loc);
  Expr *checkDecimalStatic(CallExpr *call, MemberExpr *member);
  Expr *checkDecimalMethod(CallExpr *call, MemberExpr *member);
  Expr *checkRoundingMode(Expr *arg);
  Expr *checkDecimalCount(Expr *arg, llvm::StringRef what, int64_t low, int64_t high);

private:
  using ResultRoundingMode = Result<int, DecimalCheckerRoundingModeError>;
  using ResultLiteralRange = Result<DecimalLiteral, DecimalCheckerLiteralRangeError>;

  bool isLiteralNode(const Expr *expr) const;
  bool isArithmeticOperator(BinaryOp op) const;
  bool isComparedLiteral(const Expr *expr) const;
  const Type *comparisonHint(const Expr *other, const Expr *self) const;
  ResultLiteralRange checkLiteralRange(const Expr *literal) const;
  ResultRoundingMode checkRoundingModeText(const std::u16string &text) const;
  Expr *checkDecimalPower(BinaryExpr *binary);
  bool convertDecimalSides(BinaryExpr *binary);
  bool convertDecimalSide(Expr *&side);
  void walkDecimalPowerAssign(AssignExpr *assign);
  bool isDecimalText(const Type *type) const;
  bool isStringLiteralType(const Type *type) const;
  Expr *checkDecimalText(CallExpr *call);
  bool reportBadDecimalText(llvm::ArrayRef<std::string> texts, const Expr *arg);
  bool isDecimalNumberText(const std::string &text) const;
  bool takeDecimalSpread(Expr *&arg);
  void walkKnownModes(llvm::ArrayRef<std::string> modes, const Expr *arg);
  Expr *reportDecimalMember(CallExpr *call, MemberExpr *member);
  llvm::StringRef replacementFor(llvm::StringRef name) const;
  Expr *checkDecimalArg(Expr *arg, DecimalArg kind, Builtin builtin);
  const Type *decimalResultType(DecimalResult result) const;

  SemanticState &state;
  Scopes *scopes = nullptr;
  Prelude *prelude = nullptr;
  ExprChecker *exprChecker = nullptr;
  IndexProofs *indexProofs = nullptr;
  LiteralTypes *literalTypes = nullptr;
  TypeRelations *typeRelations = nullptr;
  BinaryChecker *binaryChecker = nullptr;
  ConstantFolder *constantFolder = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
};

}
