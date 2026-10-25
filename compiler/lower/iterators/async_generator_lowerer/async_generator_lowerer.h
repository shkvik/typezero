#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// Async generators and for await.
class AsyncGeneratorLowerer {
public:
  explicit AsyncGeneratorLowerer(Lowering &lowering);

  // Async generators and `for await` (codegen_async_iter.cpp).
  llvm::Constant *asyncGeneratorLayout();
  void emitAsyncGenerator(FunctionDecl *fn, llvm::Function *function);
  // The body is done, with an owned error or null.
  void finishAsyncGenerator(llvm::Value *error);
  // `await` of an owned value of `type`: what it gives, owned.
  llvm::Value *awaitValueOf(llvm::Value *value, const Type *type, llvm::SMLoc loc);
  void agenYield(llvm::Value *value, const Type *type, llvm::SMLoc loc);
  llvm::Value *emitAsyncYield(YieldExpr *yield);
  void emitAsyncYieldAll(YieldExpr *yield, const Type *yielded);
  llvm::Value *emitAsyncStep(llvm::Value *slot, const Type *element, llvm::SMLoc loc,
                             llvm::BasicBlock *endBlock);
  // return() of the async iterator in `slot`, if any, awaited.
  void closeAsyncIterator(llvm::Value *slot);
  void emitForAwait(ForOfStmt *stmt);
  // `return x` of an async generator awaits x, which `for await` drops.
  void emitAsyncGeneratorReturn(ReturnStmt *stmt);

private:
  Lowering &lowering_;
  llvm::Constant *agenLayout_ = nullptr;
};

} // namespace tz
