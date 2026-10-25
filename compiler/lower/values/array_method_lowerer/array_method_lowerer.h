#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// The methods of arrays.
class ArrayMethodLowerer {
public:
  explicit ArrayMethodLowerer(Lowering &lowering);

  llvm::Value *emitArrayMethod(CallExpr *call);
  // A value converted to a string by the runtime, for arrays and optionals.
  llvm::Value *emitValueToString(llvm::Value *value, const Type *type);

private:
  Lowering &lowering_;
};

} // namespace tz
