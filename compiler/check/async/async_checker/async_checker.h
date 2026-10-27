#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class AsyncChecker {
public:
  explicit AsyncChecker(Checker &checker);

  void checkForAwait(ForOfStmt *stmt);
  const Type *resolvePromiseType(const TypeRef &ref);
  const Type *asyncReturnType(const FunctionDecl *fn, const Type *declared);
  const Type *inferAsyncType(const FuncSymbol *fn, llvm::ArrayRef<ReturnStmt *> returns,
                             bool fallsOff, bool report);
  void finishAsyncReturns(FuncSymbol *fn, llvm::ArrayRef<ReturnStmt *> returns);
  Expr *checkAwait(AwaitExpr *await, const Type *expected);
  static const Type *awaitedType(const Type *type);
  void checkDiscardedPromise(const Expr *expr, bool callback, llvm::SMLoc at);

private:
  Checker &checker_;
};

}
