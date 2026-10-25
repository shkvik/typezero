#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Async functions as state machines, and await.
class AsyncLowerer {
public:
  explicit AsyncLowerer(CodeGen &codegen);

  llvm::FunctionCallee asyncFunction(llvm::StringRef name, llvm::Type *ret,
                                     llvm::ArrayRef<llvm::Type *> params);
  void emitAsyncFunction(FunctionDecl *fn, llvm::Function *function);
  llvm::Function *emitAsyncMain(SourceFile &file);
  void beginAsync();
  void finishAsync();
  llvm::Function *asyncResumeFunction();
  // `return` in an async function, and where it ends: its promise is
  // fulfilled with `value` (owned, null for none).
  void emitAsyncReturnStmt(ReturnStmt *stmt);
  void emitAsyncReturn(llvm::Value *value);
  // An error leaves the body: it rejects the promise.
  void emitAsyncUnwind();
  llvm::Constant *asyncFrameDescriptor(llvm::SMLoc loc);
  void suspendAsync(llvm::Value *promise, llvm::SMLoc loc);
  // Waits for the owned `promise` of `type`: its value, owned, or its error
  // thrown from `loc`.
  llvm::Value *awaitPromise(llvm::Value *promise, const Type *type, llvm::SMLoc loc);
  // Whether a value of `type` may be a Promise: a union with one.
  static bool holdsPromise(const Type *type);
  llvm::Value *awaitUnion(llvm::Value *value, const Type *type, const Type *result,
                          llvm::SMLoc loc);
  llvm::Value *emitAwait(AwaitExpr *await);

private:
  CodeGen &codegen_;
  llvm::Function *asyncResumeFn_ = nullptr;
  const AwaitExpr *awaiting_ = nullptr;            // the await being emitted
};

} // namespace tz
