#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ResultAsyncLowerer {
public:
  explicit ResultAsyncLowerer(Lowering &lowering);

  llvm::Value *emitResultAsync(CallExpr *call);
  llvm::Value *emitResultAsyncOf(CallExpr *call, llvm::ArrayRef<llvm::Value *> values);
  llvm::Value *emitResultAsyncThrowable(CallExpr *call);

private:
  Lowering &lowering_;
};

}
