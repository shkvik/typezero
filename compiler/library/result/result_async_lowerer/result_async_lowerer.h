#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// ResultAsync of neverthrow.
class ResultAsyncLowerer {
public:
  explicit ResultAsyncLowerer(CodeGen &codegen);

  llvm::Value *emitResultAsync(CallExpr *call);
  llvm::Value *emitResultAsyncOf(CallExpr *call, llvm::ArrayRef<llvm::Value *> values);
  llvm::Value *emitResultAsyncThrowable(CallExpr *call);

private:
  CodeGen &codegen_;
};

} // namespace tz
