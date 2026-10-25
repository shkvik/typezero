#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Async functions, await, for await, and the promises nothing waits for (docs/async.md).
class AsyncChecker {
public:
  explicit AsyncChecker(Sema &sema);

  void checkForAwait(ForOfStmt *stmt);
  // Async (sema_async.cpp, docs/async.md). `Promise<T>` in an annotation, or
  // null for another name.
  const Type *resolvePromiseType(const TypeRef &ref);
  // Checks the declared return type of an async function: a Promise.
  const Type *asyncReturnType(const FunctionDecl *fn, const Type *declared);
  // The type of an async function from its returns, whose Promises give
  // their values; null if nothing is known yet (in a trial).
  const Type *inferAsyncType(const FuncSymbol *fn, llvm::ArrayRef<ReturnStmt *> returns,
                             bool fallsOff, bool report);
  // Once the type is known: each `return` gives a value of it, or a Promise
  // of it.
  void finishAsyncReturns(FuncSymbol *fn, llvm::ArrayRef<ReturnStmt *> returns);
  Expr *checkAwait(AwaitExpr *await, const Type *expected);
  // What `await` of a value of `type` gives: the value of a Promise, of each
  // Promise a union may hold with the other members, or the value itself;
  // null when those make no type.
  static const Type *awaitedType(const Type *type);
  // A Promise whose rejection something handles: `p.catch(f)`, `p.then(f,
  // g)`, and finally() after those.
  bool isHandledPromise(const Expr *expr) const;
  void checkDiscardedPromise(const Expr *expr, bool callback, llvm::SMLoc at);

private:
  Sema &sema_;
};

} // namespace tz
