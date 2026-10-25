#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// WeakRef.
class WeakRefLowerer {
public:
  explicit WeakRefLowerer(Lowering &lowering);

  // `new WeakRef(x)` and `w.deref()`.
  llvm::Value *emitWeakRefCall(CallExpr *call);

private:
  Lowering &lowering_;
};

} // namespace tz
