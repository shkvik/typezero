#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class EqualityLowerer {
public:
  explicit EqualityLowerer(Lowering &lowering);

  llvm::Value *emitEquality(BinaryExpr *bin);
  llvm::Value *emitNullish(BinaryExpr *bin);
  llvm::Value *emitValuesEqual(llvm::Value *a, llvm::Value *b, const Type *type);

private:
  Lowering &lowering_;
};

}
