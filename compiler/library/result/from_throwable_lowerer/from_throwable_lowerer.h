#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// fromThrowable and _unsafeUnwrap of neverthrow.
class FromThrowableLowerer {
public:
  explicit FromThrowableLowerer(Lowering &lowering);

  // Result.fromThrowable(f, toError?), _unsafeUnwrap() and _unsafeUnwrapErr().
  llvm::Value *emitFromThrowable(CallExpr *call);
  llvm::Value *emitUnsafeUnwrap(CallExpr *call);

private:
  Lowering &lowering_;
};

} // namespace tz
