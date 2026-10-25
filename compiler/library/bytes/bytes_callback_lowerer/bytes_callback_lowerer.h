#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// The methods of typed arrays that take a callback.
class BytesCallbackLowerer {
public:
  explicit BytesCallbackLowerer(Lowering &lowering);

  // forEach, map, filter, reduce... of a typed array (see emitCallbackMethod).
  llvm::Value *emitBytesCallbackMethod(CallExpr *call);

private:
  Lowering &lowering_;
};

} // namespace tz
