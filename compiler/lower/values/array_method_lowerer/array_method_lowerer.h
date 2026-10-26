#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ArrayMethodLowerer {
public:
  explicit ArrayMethodLowerer(Lowering &lowering);

  llvm::Value *emitArrayMethod(CallExpr *call);
  llvm::Value *emitValueToString(llvm::Value *value, const Type *type);

private:
  Lowering &lowering_;
};

}
