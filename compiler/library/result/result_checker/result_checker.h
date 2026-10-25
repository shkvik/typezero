#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Result of neverthrow, built in (docs/errors.md).
class ResultChecker {
public:
  explicit ResultChecker(Sema &sema);

  // ok(v), err(e) and safeTry(...); null if `name` is none of them.
  Expr *checkResultCall(CallExpr *call, NameExpr *name, const Type *expected);
  // Result.combine and Result.combineWithAllErrors; null if `member` is not
  // a member of Result.
  Expr *checkResultStatic(CallExpr *call, MemberExpr *member);
  // `r.value` and `r.error`.
  Expr *checkResultMember(MemberExpr *member);
  // ok(v) and err(e).
  Expr *checkOkErr(CallExpr *call, NameExpr *name, bool isOk, const Type *expected);
  // Reports a value that is computed and dropped if it is a Result: its
  // error would go unchecked. `callback`: what a function returns that its
  // caller drops. `at` is where its statement starts, for hints.
  void checkDiscarded(const Expr *expr, bool callback = false, llvm::SMLoc at = {});
  // `r.isOk()` and `r.isErr()` of a Result variable: what holds when it is
  // true and when it is false.
  void addResultFacts(const Expr *cond, Sema::Flow &whenTrue, Sema::Flow &whenFalse) const;
  // Result.fromThrowable(f, toError?) and neverthrow's fromThrowable
  // (sema_result.cpp); `loc` is where the callee is.
  Expr *checkFromThrowable(CallExpr *call, llvm::SMLoc loc);

private:
  Sema &sema_;
};

} // namespace tz
