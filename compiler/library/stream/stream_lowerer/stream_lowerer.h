#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// The intrinsics of node:stream.
class StreamLowerer {
public:
  explicit StreamLowerer(CodeGen &codegen);

  // The intrinsics of node:stream (codegen_stream.cpp): LibChunk,
  // LibIsChunk and LibOverrides. emitChunkAs: the value of `to` the unknown
  // `u` (borrowed) holds, owned, going to `fail` if it holds none.
  llvm::Value *emitStreamBuiltin(CallExpr *call);
  llvm::Value *emitChunkAs(llvm::Value *u, const Type *to, llvm::BasicBlock *fail);

private:
  CodeGen &codegen_;
};

} // namespace tz
