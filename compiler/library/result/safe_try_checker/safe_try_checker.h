#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class SafeTryChecker {
public:
  explicit SafeTryChecker(Checker &checker);

  Expr *checkSafeTry(CallExpr *call, const Type *expected);
  Expr *checkSafeTryYield(YieldExpr *yield);
  const Type *generatorResult(const FunctionDecl *fn, const Type *returned);
  void checkYieldPlacement(const Stmt *stmt);
  void checkYieldPlacement(const Expr *expr, bool safe);

private:
  Checker &checker_;
};

}
