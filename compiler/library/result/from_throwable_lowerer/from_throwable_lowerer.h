#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class FromThrowableLowerer {
public:
  explicit FromThrowableLowerer(Lowering &lowering);

  llvm::Value *emitFromThrowable(CallExpr *call);
  llvm::Value *emitUnsafeUnwrap(CallExpr *call);

private:
  Lowering &lowering_;
};

}
