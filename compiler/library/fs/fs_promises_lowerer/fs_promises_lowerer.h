#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// node:fs/promises, node:readline, node:timers/promises and AbortSignal.
class FsPromisesLowerer {
public:
  explicit FsPromisesLowerer(Lowering &lowering);

  // node:fs/promises and node:readline; node:timers/promises
  // (codegen_async_io.cpp).
  llvm::Value *emitFsPromise(CallExpr *call);
  llvm::Value *emitReadlineBuiltin(CallExpr *call);
  llvm::Value *emitAsyncIoBuiltin(CallExpr *call);

private:
  Lowering &lowering_;
};

} // namespace tz
