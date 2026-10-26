#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class WeakRefLowerer {
public:
  explicit WeakRefLowerer(Lowering &lowering);

  llvm::Value *emitWeakRefCall(CallExpr *call);

private:
  Lowering &lowering_;
};

}
