#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// The methods of arrays that take a callback, and sort with a comparator.
class CallbackMethodLowerer {
public:
  explicit CallbackMethodLowerer(Lowering &lowering);

  // Methods of arrays that take a callback (codegen_array.cpp).
  llvm::Value *emitCallbackMethod(CallExpr *call);
  llvm::Function *sortComparator(CallExpr *call);

private:
  Lowering &lowering_;
};

} // namespace tz
