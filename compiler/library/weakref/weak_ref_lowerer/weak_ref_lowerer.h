#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class WeakRefLowerer {
public:
  explicit WeakRefLowerer(Lowering &owner);

  llvm::Value *emitWeakRefCall(CallExpr *call);

private:
  Lowering &lowering;
};

}
