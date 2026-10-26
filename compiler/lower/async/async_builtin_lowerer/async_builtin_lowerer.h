#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class AsyncBuiltinLowerer {
public:
  explicit AsyncBuiltinLowerer(Lowering &lowering);

  llvm::Value *emitAsyncBuiltin(CallExpr *call);

private:
  Lowering &lowering_;
};

}
