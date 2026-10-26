#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ReactionLowerer {
public:
  explicit ReactionLowerer(Lowering &lowering);

  llvm::Value *emitPromiseReaction(CallExpr *call);
  llvm::StructType *reactionType();
  llvm::Function *reactionDrop();
  llvm::Function *reactionHandler(Builtin kind, const Type *value, const Type *result,
                                  const Type *onFulfilled, const Type *onRejected);
  void attachReaction(llvm::Value *source, llvm::Value *derived, llvm::Function *handler,
                      llvm::Value *onFulfilled, llvm::Value *onRejected, bool later = false);

private:
  Lowering &lowering_;
  llvm::StructType *reactionTy_ = nullptr;
  llvm::Function *reactionDropFn_ = nullptr;
};

}
