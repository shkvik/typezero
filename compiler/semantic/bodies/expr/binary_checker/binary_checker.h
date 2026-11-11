#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/expr/binary_checker/binary_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

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
  Expr *checkEquality(BinaryExpr *bin);
  Expr *checkBinary(BinaryExpr *binary, const Type *expected, bool wraps);
  ResultNullishResolved checkNullish(BinaryExpr *bin, const Type *expected);
  ResultTypeOfNameKnown checkTypeOfName(const Expr *operand, const StringLitExpr *text);
  static std::string operandTypesText(llvm::StringRef op, const Type *lhs, const Type *rhs);

private:
  struct OperandHints {
    const Type *hint = nullptr;
    const Type *rhsHint = nullptr;
    bool bitwise = false;
    bool literals = false;
  };

  using ResultBitwiseResolved = Result<Expr *, BinaryCheckerCheckBitsError>;
  using ResultWrappingChecked = Result<Expr *, BinaryCheckerCheckWrappingError>;
  using ResultExponentIsValid = Result<const Expr *, BinaryCheckerPowerExponentError>;
  using ResultNoResultOperand = Result<const BinaryExpr *, BinaryCheckerNoResultsError>;

  void adaptLiterals(BinaryExpr *bin);
  static bool onlyIntLiterals(const Expr *expr);
  static bool isIntLiteralNode(const Expr *expr);
  ResultBitwiseResolved checkBits(BinaryExpr *bin);
  static std::optional<BinaryCheckerCheckBitsErrorCode> bitsProblem(const BinaryExpr *bin,
                                                                    bool shift);
  static std::string bitsProblemText(const BinaryExpr *bin,
                                     BinaryCheckerCheckBitsErrorCode problem);
  static std::string nothingChangesText(const BinaryExpr *bin);
  static std::string doubleBitsText(const BinaryExpr *bin);
  static std::string signedShiftText(const BinaryExpr *bin);
  ResultBitwiseResolved checkWrappingBits(BinaryExpr *bin);
  static ResultTypeOfNameKnown unknownTypeOfName(const Expr *operand, const std::string &name);
  static ResultTypeOfNameKnown noNumberName(const Expr *operand);
  static std::vector<llvm::StringRef> numberTypeOfNames(const Type *type);
  ResultTypeOfNameKnown typeOfNameMatches(const Expr *operand, const StringLitExpr *text,
                                          const std::string &name) const;
  void walkTypeOfSide(const Expr *side, const Expr *other);
  Expr *checkEqualOperands(BinaryExpr *bin);
  void reportIncomparable(const BinaryExpr *bin);
  ResultWrappingChecked checkWrapping(BinaryExpr *bin);
  static std::string mixedNumbersText(const BinaryExpr *bin);
  static ResultNoResultOperand noResults(const BinaryExpr *bin);
  static std::string quotedNames(llvm::ArrayRef<llvm::StringRef> names);
  ResultExponentIsValid powerExponent(const Expr *rhs, const Type *common) const;
  static const Type *integerHint(const Expr *literal, const Expr *other, const Type *hint,
                                 bool bitwise);

  static bool isBitwise(BinaryOp op);
  static bool joinsAsText(const Type *type);
  Expr *checkComparison(BinaryExpr *bin);
  Expr *checkInOperator(BinaryExpr *bin);
  Expr *checkConcatenation(BinaryExpr *bin);
  Expr *checkBitwiseOperator(BinaryExpr *bin);
  Expr *checkNumberComparison(BinaryExpr *bin);
  Expr *checkInstanceOfOperator(BinaryExpr *bin);
  Expr *checkWrappingArithmetic(BinaryExpr *bin);
  Expr *checkShift(BinaryExpr *bin, const Type *expected);
  Expr *checkArithmeticResult(BinaryExpr *bin, bool wraps);
  Expr *checkNumberArithmetic(BinaryExpr *bin, bool wraps);
  void walkArithmeticOperands(BinaryExpr *bin, const Type *expected);
  Expr *checkNullishOperator(BinaryExpr *bin, const Type *expected);
  Expr *checkLogicalOperator(BinaryExpr *bin, const Type *expected);
  void walkLeftOperandFirst(BinaryExpr *bin, const OperandHints &hints);
  void walkRightOperandFirst(BinaryExpr *bin, const OperandHints &hints);
  Expr *checkArithmetic(BinaryExpr *bin, const Type *expected, bool wraps);
  static OperandHints operandHints(const BinaryExpr *bin, const Type *expected);
  Expr *checkIntegerArithmetic(BinaryExpr *bin, const Type *common, bool wraps);
  void trackMixedConcat(BinaryExpr *bin, bool chainEnd, const BinaryExpr *outerMixed);
  static llvm::SMLoc inErrorPlace(const BinaryExpr *bin, UnaryCheckerCheckInErrorCode code);
  static llvm::SMLoc instanceOfErrorPlace(const BinaryExpr *bin,
                                          InstanceOfCheckerCheckInstanceOfErrorCode code);

  SemanticState &state;
  DateChecker *dateChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  JsonChecker *jsonChecker = nullptr;
  ResultTypes *resultTypes = nullptr;
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
