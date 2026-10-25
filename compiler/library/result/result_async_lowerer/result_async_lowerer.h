#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// ResultAsync of neverthrow.
class ResultAsyncLowerer {
public:
  explicit ResultAsyncLowerer(Lowering &lowering);

  llvm::Value *emitResultAsync(CallExpr *call);
  llvm::Value *emitResultAsyncOf(CallExpr *call, llvm::ArrayRef<llvm::Value *> values);
  llvm::Value *emitResultAsyncThrowable(CallExpr *call);

private:
  Lowering &lowering_;
};

} // namespace tz
