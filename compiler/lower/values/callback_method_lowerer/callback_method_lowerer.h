#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class CallbackMethodLowerer {
public:
  explicit CallbackMethodLowerer(Lowering &lowering);

  llvm::Value *emitCallbackMethod(CallExpr *call);
  llvm::Function *sortComparator(CallExpr *call);

private:
  Lowering &lowering_;
};

}
