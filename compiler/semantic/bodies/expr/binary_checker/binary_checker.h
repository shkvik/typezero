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
  using ResultBitwiseResolved = Result<Expr *, BinaryCheckerCheckBitsError>;
  using ResultWrappingChecked = Result<Expr *, BinaryCheckerCheckWrappingError>;
  using ResultExponentIsValid = Result<const Expr *, BinaryCheckerPowerExponentError>;
  using ResultNoResultOperand = Result<const BinaryExpr *, BinaryCheckerNoResultsError>;

  void adaptLiterals(BinaryExpr *bin);
  static bool onlyIntLiterals(const Expr *expr);
  static bool isIntLiteralNode(const Expr *expr);
  ResultBitwiseResolved checkBits(BinaryExpr *bin);
  ResultWrappingChecked checkWrapping(BinaryExpr *bin);
  static std::string mixedNumbersText(const BinaryExpr *bin);
  static ResultNoResultOperand noResults(const BinaryExpr *bin);
  static std::string quotedNames(llvm::ArrayRef<llvm::StringRef> names);
  ResultExponentIsValid powerExponent(const Expr *rhs, const Type *common) const;
  static const Type *integerHint(const Expr *literal, const Expr *other, const Type *hint,
                                 bool bitwise);

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
