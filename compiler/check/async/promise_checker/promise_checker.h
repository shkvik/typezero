#pragma once

#include "compiler/check/async/promise_checker/promise_checker_errors.h"
#include "compiler/check/checker/checker.h"
#include "compiler/result/result/result.h"

namespace tz {

class PromiseChecker {
public:
  using ResultPromiseChained = Result<Expr *, PromiseCheckerCheckPromiseMethodError>;

  explicit PromiseChecker(Checker &owner);

  static const Type *resolverType(const Type *value);
  const Type *executorValueType(const Expr *executor);
  Expr *checkPromiseNew(NewExpr *expr, const Type *expected);
  ResultPromiseChained checkPromiseMethod(CallExpr *call, MemberExpr *member, const Type *expected);

private:
  using ResultValueTypeKnown = Result<const Type *, PromiseCheckerValueTypeError>;
  using ResultExecutorShaped = Result<const Expr *, PromiseCheckerExecutorShapeError>;

  static const Type *awaited(const Type *type);
  static ResultExecutorShaped executorShape(const Expr *executor);
  ResultValueTypeKnown valueType(NewExpr *expr, const CallExpr *call, const Type *expected);

  Checker &checker;
};

}
