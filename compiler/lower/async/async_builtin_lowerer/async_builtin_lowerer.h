#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// Timers, queueMicrotask, nextTick and the other built-ins of async code.
class AsyncBuiltinLowerer {
public:
  explicit AsyncBuiltinLowerer(Lowering &lowering);

  llvm::Value *emitAsyncBuiltin(CallExpr *call);

private:
  Lowering &lowering_;
};

} // namespace tz
