#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class StringMethodLowerer {
public:
  explicit StringMethodLowerer(Lowering &owner);

  llvm::Value *emitStringMethod(CallExpr *call);

private:
  Lowering &lowering;
};

}
