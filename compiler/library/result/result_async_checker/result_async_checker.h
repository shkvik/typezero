#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// ResultAsync of neverthrow.
class ResultAsyncChecker {
public:
  explicit ResultAsyncChecker(Checker &checker);

  // ResultAsync (sema_async.cpp): okAsync, errAsync, fromPromise,
  // fromSafePromise and fromAsyncThrowable, called by the name `which`
  // stands for; the functions of ResultAsync (null if `member` is not one);
  // the methods of a ResultAsync, and asyncMap and asyncAndThen of a Result
  // (`name`, which the method is then).
  Expr *checkResultAsyncCall(CallExpr *call, NameExpr *name, llvm::StringRef which,
                             const Type *expected);
  Expr *checkResultAsyncStatic(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkResultFromPromise(CallExpr *call, llvm::SMLoc loc, bool safe, const Type *expected);
  Expr *checkResultAsyncThrowable(CallExpr *call, llvm::SMLoc loc);
  Expr *checkResultAsyncMethod(CallExpr *call, MemberExpr *member, llvm::StringRef name);

private:
  Checker &checker_;
};

} // namespace tz
