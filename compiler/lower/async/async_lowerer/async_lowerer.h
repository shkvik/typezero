#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class AsyncLowerer {
public:
  explicit AsyncLowerer(Lowering &owner);

  llvm::FunctionCallee asyncFunction(llvm::StringRef name, llvm::Type *ret,
                                     llvm::ArrayRef<llvm::Type *> params);
  void emitAsyncFunction(FunctionDecl *fn, llvm::Function *function);
  llvm::Function *emitAsyncMain(SourceFile &file);
  void beginAsync();
  void finishAsync();
  llvm::Function *asyncResumeFunction();
  void emitAsyncReturnStmt(ReturnStmt *stmt);
  void emitAsyncReturn(llvm::Value *value);
  void suspendAsync(llvm::Value *promise, llvm::SMLoc loc);
  llvm::Value *awaitPromise(llvm::Value *promise, const Type *type, llvm::SMLoc loc);
  static bool holdsPromise(const Type *type);
  llvm::Value *awaitUnion(llvm::Value *value, const Type *type, const Type *result,
                          llvm::SMLoc loc);
  llvm::Value *emitAwait(AwaitExpr *await);

private:
  static const Expr *resolvedValue(const Expr *expr);

  Lowering &lowering;
  llvm::Function *asyncResumeFn = nullptr;
  const AwaitExpr *awaiting = nullptr;
  bool readyGoesOn = false;
};

}
