#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/check/functions/closure_checker/closure_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class ClosureChecker {
public:
  explicit ClosureChecker(Checker &owner);

  static Checker::Signature signatureOf(const Type *fnType);
  Expr *checkFunctionExpr(FunctionExpr *fe, const Checker::Signature *expected);
  void checkClosureBody(FunctionDecl *fn);
  void declareOwnName(FunctionDecl *fn);
  void checkStatements(BlockStmt *block);

private:
  using ResultExprNotGeneric = Result<const FunctionDecl *, ClosureCheckerNotGenericError>;
  using ResultParamTypeAgree = Result<const Type *, ClosureCheckerParamAgreesError>;
  using ResultDefaultMatters = Result<const Param *, ClosureCheckerDefaultMattersError>;
  using ResultParamTypeKnown = Result<const Param *, ClosureCheckerParamTypeKnownError>;
  using ResultThisParamValid = Result<const Param *, ClosureCheckerThisParamError>;
  using ResultRestParamArray = Result<const Type *, ClosureCheckerRestParamError>;
  using ResultParamOrderGood = Result<const Param *, ClosureCheckerParamOrderError>;
  using ResultPromiseAwaited =
      Result<const Checker::Signature *, ClosureCheckerPromiseAwaitedError>;

  static ResultExprNotGeneric notGeneric(const FunctionDecl *fn);
  static ResultParamTypeAgree paramAgrees(const Param &param, const Type *passed,
                                          const Type *given);
  static ResultDefaultMatters defaultMatters(const Param &param, const Type *given);
  static ResultParamTypeKnown paramTypeKnown(const Param &param);
  static ResultThisParamValid thisParam(const FunctionDecl *fn, const Param &param);
  static ResultRestParamArray restParam(const Param &param, const Type *type);
  static ResultParamOrderGood paramOrder(const Param &param, bool afterOptional);
  static ResultPromiseAwaited promiseAwaited(const Checker::Signature *sig);

  Checker &checker;
};

}
