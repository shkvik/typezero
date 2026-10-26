#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class CallLowerer {
public:
  explicit CallLowerer(Lowering &lowering);

  llvm::Value *emitCall(CallExpr *call);

private:
  Lowering &lowering_;
};

}
