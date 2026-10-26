#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class StreamLowerer {
public:
  explicit StreamLowerer(Lowering &lowering);

  llvm::Value *emitStreamBuiltin(CallExpr *call);
  llvm::Value *emitChunkAs(llvm::Value *u, const Type *to, llvm::BasicBlock *fail);

private:
  Lowering &lowering_;
};

}
