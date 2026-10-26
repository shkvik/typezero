#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ResultAsyncChecker {
public:
  explicit ResultAsyncChecker(Checker &checker);

  Expr *checkResultAsyncCall(CallExpr *call, NameExpr *name, llvm::StringRef which,
                             const Type *expected);
  Expr *checkResultAsyncStatic(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkResultFromPromise(CallExpr *call, llvm::SMLoc loc, bool safe, const Type *expected);
  Expr *checkResultAsyncThrowable(CallExpr *call, llvm::SMLoc loc);
  Expr *checkResultAsyncMethod(CallExpr *call, MemberExpr *member, llvm::StringRef name);

private:
  Checker &checker_;
};

}
