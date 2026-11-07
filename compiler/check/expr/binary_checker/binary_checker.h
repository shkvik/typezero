#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/check/expr/binary_checker/binary_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class BinaryChecker {
public:
  using ResultNullishResolved = Result<Expr *, BinaryCheckerCheckNullishError>;
  using ResultTypeOfNameKnown = Result<const StringLitExpr *, BinaryCheckerCheckTypeOfNameError>;

  explicit BinaryChecker(Checker &owner);

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
  ResultBitwiseResolved checkBits(BinaryExpr *bin);
  ResultWrappingChecked checkWrapping(BinaryExpr *bin);
  static std::string mixedNumbersText(const BinaryExpr *bin);
  static ResultNoResultOperand noResults(const BinaryExpr *bin);
  static std::string quotedNames(llvm::ArrayRef<llvm::StringRef> names);
  ResultExponentIsValid powerExponent(const Expr *rhs, const Type *common) const;
  static const Type *integerHint(const Expr *literal, const Expr *other, const Type *hint,
                                 bool bitwise);

  Checker &checker;
};

}
