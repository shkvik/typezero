#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class PromiseChecker {
public:
  explicit PromiseChecker(Checker &owner);

  static const Type *resolverType(const Type *value);
  const Type *executorValueType(const Expr *executor);
  Expr *checkPromiseNew(NewExpr *expr, const Type *expected);
  Expr *checkPromiseMethod(CallExpr *call, MemberExpr *member, const Type *expected);

private:
  static const Type *awaited(const Type *type);

  Checker &checker;
};

}
