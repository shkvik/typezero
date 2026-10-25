#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// then, catch and finally: the reactions of promises.
class ReactionLowerer {
public:
  explicit ReactionLowerer(Lowering &lowering);

  llvm::Value *emitPromiseReaction(CallExpr *call);
  llvm::StructType *reactionType();
  llvm::Function *reactionDrop();
  llvm::Function *reactionHandler(Builtin kind, const Type *value, const Type *result,
                                  const Type *onFulfilled, const Type *onRejected);
  // A new reaction to `source` with `handler`, which settles `derived`
  // (borrowed) and holds the owned callbacks (null for none). With `later`
  // it waits for `source` from the next turn of the queue on.
  void attachReaction(llvm::Value *source, llvm::Value *derived, llvm::Function *handler,
                      llvm::Value *onFulfilled, llvm::Value *onRejected, bool later = false);

private:
  Lowering &lowering_;
  llvm::StructType *reactionTy_ = nullptr;
  llvm::Function *reactionDropFn_ = nullptr;
};

} // namespace tz
