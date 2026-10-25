#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// Result.combine.
class ResultCombineLowerer {
public:
  explicit ResultCombineLowerer(Lowering &lowering);

  llvm::Value *emitCombine(CallExpr *call);
  llvm::Value *emitCombineTuple(CallExpr *call);

private:
  Lowering &lowering_;
};

} // namespace tz
