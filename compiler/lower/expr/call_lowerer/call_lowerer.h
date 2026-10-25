#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// Calls: which class builds a call of a built-in, a method or a function.
class CallLowerer {
public:
  explicit CallLowerer(Lowering &lowering);

  llvm::Value *emitCall(CallExpr *call);

private:
  Lowering &lowering_;
};

} // namespace tz
