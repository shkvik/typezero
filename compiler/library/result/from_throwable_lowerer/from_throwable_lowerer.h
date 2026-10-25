#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// fromThrowable and _unsafeUnwrap of neverthrow.
class FromThrowableLowerer {
public:
  explicit FromThrowableLowerer(CodeGen &codegen);

  // Result.fromThrowable(f, toError?), _unsafeUnwrap() and _unsafeUnwrapErr().
  llvm::Value *emitFromThrowable(CallExpr *call);
  llvm::Value *emitUnsafeUnwrap(CallExpr *call);

private:
  CodeGen &codegen_;
};

} // namespace tz
