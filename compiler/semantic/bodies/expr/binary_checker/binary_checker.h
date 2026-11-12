#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/expr/binary_checker/binary_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class FlowChecker;
class SemanticParts;
class DateChecker;
class ExprChecker;
class JsonChecker;
class ResultTypes;
class MatchChecker;
class TruthChecker;
class UnaryChecker;
class ConcatChecker;
class TypeRelations;
class ConstantFolder;
class DecimalChecker;
class GenericChecker;
class InstanceOfChecker;

enum class UnaryCheckerCheckInErrorCode;
enum class InstanceOfCheckerCheckInstanceOfErrorCode;

class BinaryChecker {
public:
  using ResultNullishResolved = Result<Expr *, BinaryCheckerCheckNullishError>;
  using ResultTypeOfNameKnown = Result<const StringLitExpr *, BinaryCheckerCheckTypeOfNameError>;

  BinaryChecker(SemanticState &owner, SemanticParts &all);

  Expr *toInt32(Expr *expr);
  Expr *checkEquality(BinaryExpr *binary);
  Expr *checkBinary(BinaryExpr *binary, const Type *expected);
  ResultNullishResolved checkNullish(BinaryExpr *binary, const Type *expected);
  ResultTypeOfNameKnown checkTypeOfName(const Expr *operand, const StringLitExpr *text);
  std::string operandTypesText(llvm::StringRef op, const Type *lhs, const Type *rhs) const;

private:
  struct OperandHints {
    const Type *hint = nullptr;
    const Type *rhsHint = nullptr;
    bool isBitwise = false;
    bool canHintLiterals = false;
  };

  struct NumberConversion {
    const Type *target = nullptr;
    bool isLeftConverted = false;
  };

  using ResultBitwiseResolved = Result<Expr *, BinaryCheckerCheckBitsError>;
  using ResultWrappingChecked = Result<Expr *, BinaryCheckerCheckWrappingError>;
  using ResultExponentIsValid = Result<const Expr *, BinaryCheckerPowerExponentError>;
  using ResultNoResultOperand = Result<const BinaryExpr *, BinaryCheckerNoResultsError>;

  bool hasOnlyIntLiterals(const Expr *expr) const;
  bool isIntLiteralNode(const Expr *expr) const;
  const Type *integerHint(const Expr *literal, const Expr *other, const Type *hint,
                          bool isBitwise) const;
  ResultExponentIsValid checkPowerExponent(const Expr *rhs, const Type *common) const;
  Expr *checkInOperator(BinaryExpr *binary);
  llvm::SMLoc inErrorPlace(const BinaryExpr *binary, UnaryCheckerCheckInErrorCode code) const;
  Expr *checkInstanceOfOperator(BinaryExpr *binary);
  llvm::SMLoc instanceOfErrorPlace(const BinaryExpr *binary,
                                   InstanceOfCheckerCheckInstanceOfErrorCode code) const;
  Expr *checkLogicalOperator(BinaryExpr *binary, const Type *expected);
  Expr *checkNullishOperator(BinaryExpr *binary, const Type *expected);
  Expr *checkComparison(BinaryExpr *binary);
  Expr *checkNumberComparison(BinaryExpr *binary);
  Expr *checkShift(BinaryExpr *binary, const Type *expected);
  Expr *checkBitwiseOperator(BinaryExpr *binary);
  bool isBitwiseOperator(BinaryOp op) const;
  bool isShiftOperator(BinaryOp op) const;
  Expr *checkArithmetic(BinaryExpr *binary, const Type *expected);
  OperandHints operandHints(const BinaryExpr *binary, const Type *expected) const;
  void walkArithmeticOperands(BinaryExpr *binary, const Type *expected);
  void walkRightOperandFirst(BinaryExpr *binary, const OperandHints &hints);
  void walkLeftOperandFirst(BinaryExpr *binary, const OperandHints &hints);
  void trackMixedConcat(BinaryExpr *binary, bool isChainEnd, const BinaryExpr *outerMixed);
  bool isJoiningAsText(const Type *type) const;
  Expr *checkArithmeticResult(BinaryExpr *binary);
  Expr *checkConcatenation(BinaryExpr *binary);
  Expr *checkWrappingArithmetic(BinaryExpr *binary);
  Expr *checkNumberArithmetic(BinaryExpr *binary);
  Expr *checkIntegerArithmetic(BinaryExpr *binary, const Type *common);
  ResultWrappingChecked checkWrapping(BinaryExpr *binary);
  ResultBitwiseResolved checkBits(BinaryExpr *binary);
  std::optional<BinaryCheckerCheckBitsErrorCode> bitsProblem(const BinaryExpr *binary) const;
  std::string bitsProblemText(const BinaryExpr *binary,
                              BinaryCheckerCheckBitsErrorCode problem) const;
  std::string nothingChangesText(const BinaryExpr *binary) const;
  std::string nothingChangesHint(const BinaryExpr *binary, llvm::StringRef lhsText) const;
  bool isRoundingCall(const Expr *expr) const;
  std::string shownOperand(llvm::StringRef text) const;
  std::string doubleBitsText(const BinaryExpr *binary) const;
  std::string signedShiftText(const BinaryExpr *binary) const;
  ResultBitwiseResolved checkWrappingBits(BinaryExpr *binary);
  BinaryCheckerCheckBitsErrorCode wrappingBitsCode(BinaryCheckerCheckWrappingErrorCode code) const;
  void adaptLiterals(BinaryExpr *binary);
  bool isNumberLike(const Type *type) const;
  std::string mixedNumbersText(const BinaryExpr *binary) const;
  NumberConversion mixedConversion(const Type *leftType, const Type *rightType) const;
  std::string mixedExample(const BinaryExpr *binary, const NumberConversion &conversion) const;
  std::string convertedText(llvm::StringRef operand, const std::string &method) const;
  std::string quotedNames(llvm::ArrayRef<llvm::StringRef> names) const;
  const char *nameSeparator(size_t index, size_t count) const;
  ResultTypeOfNameKnown checkKnownTypeOfName(const Expr *operand, const StringLitExpr *text,
                                             const std::string &name) const;
  std::string noNumberText(const Expr *operand) const;
  std::string numberNamesHere(const Expr *operand) const;
  std::vector<llvm::StringRef> numberTypeOfNames(const Type *type) const;
  ResultTypeOfNameKnown checkTypeOfNameMatches(const Expr *operand, const StringLitExpr *text,
                                               const std::string &name) const;
  ResultNoResultOperand checkNoResults(const BinaryExpr *binary) const;
  void walkTypeOfSide(const Expr *side, const Expr *other);
  Expr *checkEqualOperands(BinaryExpr *binary);
  void reportIncomparable(const BinaryExpr *binary);
  void walkNullishOperands(BinaryExpr *binary, const Type *expected);
  const Type *failedNullishType(const Type *leftType, const Type *rightType) const;

  SemanticState &state;
  DateChecker *dateChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  JsonChecker *jsonChecker = nullptr;
  ResultTypes *resultTypes = nullptr;
  FlowChecker *flowChecker = nullptr;
  MatchChecker *matchChecker = nullptr;
  TruthChecker *truthChecker = nullptr;
  UnaryChecker *unaryChecker = nullptr;
  ConcatChecker *concatChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ConstantFolder *constantFolder = nullptr;
  DecimalChecker *decimalChecker = nullptr;
  GenericChecker *genericChecker = nullptr;
  InstanceOfChecker *instanceOfChecker = nullptr;
};

}
