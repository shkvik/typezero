#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ReactionLowerer {
public:
  explicit ReactionLowerer(Lowering &lowering);

  llvm::Value *emitPromiseReaction(CallExpr *call);
  llvm::StructType *reactionType();
  llvm::Function *reactionDrop();
  llvm::Function *reactionHandler(const Type *value, const Type *result, const Type *callback);
  void attachReaction(llvm::Value *source, llvm::Value *derived, llvm::Function *handler,
                      llvm::Value *callback, bool later = false);

private:
  Lowering &lowering_;
  llvm::StructType *reactionTy_ = nullptr;
  llvm::Function *reactionDropFn_ = nullptr;
};

}
