#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/check/classes/method_literal_checker/method_literal_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class MethodLiteralChecker {
public:
  explicit MethodLiteralChecker(Checker &owner);

  Expr *checkMethodLiteral(ObjectLitExpr *lit, const Type *target);

private:
  using ResultSpreadAbsent = Result<const ObjectLitExpr *, MethodLiteralCheckerNoSpreadError>;
  using ResultMethodSimple = Result<const FunctionDecl *, MethodLiteralCheckerPlainMethodError>;
  using ResultValueIsTyped = Result<const Type *, MethodLiteralCheckerFieldValueError>;
  using ResultPropertyFits =
      Result<const ObjectLitExpr::Property *, MethodLiteralCheckerPropertyFitsError>;
  using ResultMembersMatch = Result<const ObjectLitExpr *, MethodLiteralCheckerMembersMatchError>;

  static ResultSpreadAbsent noSpread(const ObjectLitExpr *lit);
  static ResultMethodSimple plainMethod(const FunctionDecl *fn);
  static ResultValueIsTyped fieldValue(const Type *valueType, llvm::StringRef label);
  static ResultMembersMatch membersMatch(const ObjectLitExpr *lit, const Type *target);
  ResultPropertyFits propertyFits(const ObjectLitExpr::Property &prop, const Type *target) const;

  Checker &checker;
};

}
