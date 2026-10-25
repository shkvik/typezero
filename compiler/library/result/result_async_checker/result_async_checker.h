#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// ResultAsync of neverthrow.
class ResultAsyncChecker {
public:
  explicit ResultAsyncChecker(Sema &sema);

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
  Sema &sema_;
};

} // namespace tz
