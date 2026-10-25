#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// new Promise and the methods of promises.
class PromiseChecker {
public:
  explicit PromiseChecker(Checker &checker);

  // What Promise.allSettled gives for each: `{ status, value?, reason? }`.
  const Type *settledResultType(const Type *value);
  // The functions `new Promise` gives its executor: `(value: T) => void`
  // (`() => void` for void) and `(reason: Error) => void`.
  static const Type *resolverType(const Type *value);
  const Type *rejecterType();
  const Type *executorValueType(const Expr *executor);
  Expr *checkPromiseNew(NewExpr *expr, const Type *expected);
  Expr *checkPromiseMethod(CallExpr *call, MemberExpr *member, const Type *expected);

private:
  Checker &checker_;
};

} // namespace tz
