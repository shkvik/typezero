#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// The intrinsics of node:stream.
class StreamLowerer {
public:
  explicit StreamLowerer(Lowering &lowering);

  // The intrinsics of node:stream (codegen_stream.cpp): LibChunk,
  // LibIsChunk and LibOverrides. emitChunkAs: the value of `to` the unknown
  // `u` (borrowed) holds, owned, going to `fail` if it holds none.
  llvm::Value *emitStreamBuiltin(CallExpr *call);
  llvm::Value *emitChunkAs(llvm::Value *u, const Type *to, llvm::BasicBlock *fail);

private:
  Lowering &lowering_;
};

} // namespace tz
