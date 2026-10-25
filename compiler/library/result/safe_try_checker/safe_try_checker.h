#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// safeTry and yield* of results.
class SafeTryChecker {
public:
  explicit SafeTryChecker(Sema &sema);

  Expr *checkSafeTry(CallExpr *call, const Type *expected);
  // `yield* r` in the generator of safeTry.
  Expr *checkSafeTryYield(YieldExpr *yield);
  // The Result type of the generator `fn`: what it returns, `returned`, with
  // the errors its `yield*` may return.
  const Type *generatorResult(const FunctionDecl *fn, const Type *returned);
  // Reports each `yield*` of a generator that may return while a value
  // computed earlier in its statement is still held (see sema_result.cpp).
  void checkYieldPlacement(const Stmt *stmt);
  void checkYieldPlacement(const Expr *expr, bool safe);

private:
  Sema &sema_;
};

} // namespace tz
