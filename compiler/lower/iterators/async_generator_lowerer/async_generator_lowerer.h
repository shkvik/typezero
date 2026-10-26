#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class AsyncGeneratorLowerer {
public:
  explicit AsyncGeneratorLowerer(Lowering &lowering);

  llvm::Constant *asyncGeneratorLayout();
  void emitAsyncGenerator(FunctionDecl *fn, llvm::Function *function);
  void finishAsyncGenerator(llvm::Value *error);
  llvm::Value *awaitValueOf(llvm::Value *value, const Type *type, llvm::SMLoc loc);
  void agenYield(llvm::Value *value, const Type *type, llvm::SMLoc loc);
  llvm::Value *emitAsyncYield(YieldExpr *yield);
  void emitAsyncYieldAll(YieldExpr *yield, const Type *yielded);
  llvm::Value *emitAsyncStep(llvm::Value *slot, const Type *element, llvm::SMLoc loc,
                             llvm::BasicBlock *endBlock);
  void closeAsyncIterator(llvm::Value *slot);
  void emitForAwait(ForOfStmt *stmt);
  void emitAsyncGeneratorReturn(ReturnStmt *stmt);

private:
  Lowering &lowering_;
  llvm::Constant *agenLayout_ = nullptr;
};

}
