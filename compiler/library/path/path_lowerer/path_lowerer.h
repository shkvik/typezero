#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class PathLowerer {
public:
  explicit PathLowerer(Lowering &lowering);

  llvm::Value *emitPathCall(CallExpr *call);

private:
  Lowering &lowering_;
};

}
