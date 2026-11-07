#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class EqualityLowerer {
public:
  explicit EqualityLowerer(Lowering &owner);

  llvm::Value *emitEquality(BinaryExpr *bin);
  llvm::Value *emitNullish(BinaryExpr *bin);
  llvm::Value *emitValuesEqual(llvm::Value *a, llvm::Value *b, const Type *type);

private:
  static bool isReference(const Type *type);

  Lowering &lowering;
};

}
