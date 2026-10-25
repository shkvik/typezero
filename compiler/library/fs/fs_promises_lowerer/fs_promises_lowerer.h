#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// node:fs/promises, node:readline, node:timers/promises and AbortSignal.
class FsPromisesLowerer {
public:
  explicit FsPromisesLowerer(CodeGen &codegen);

  // node:fs/promises and node:readline; node:timers/promises
  // (codegen_async_io.cpp).
  llvm::Value *emitFsPromise(CallExpr *call);
  llvm::Value *emitReadlineBuiltin(CallExpr *call);
  llvm::Value *emitAsyncIoBuiltin(CallExpr *call);

private:
  CodeGen &codegen_;
};

} // namespace tz
