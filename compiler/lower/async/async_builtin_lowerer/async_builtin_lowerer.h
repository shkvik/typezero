#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Timers, queueMicrotask, nextTick and the other built-ins of async code.
class AsyncBuiltinLowerer {
public:
  explicit AsyncBuiltinLowerer(CodeGen &codegen);

  llvm::Value *emitAsyncBuiltin(CallExpr *call);

private:
  CodeGen &codegen_;
};

} // namespace tz
