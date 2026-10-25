#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// ==, != and ??.
class EqualityLowerer {
public:
  explicit EqualityLowerer(Lowering &lowering);

  llvm::Value *emitEquality(BinaryExpr *bin);
  llvm::Value *emitNullish(BinaryExpr *bin);
  // Whether two values of `type` are equal (==); both borrowed.
  llvm::Value *emitValuesEqual(llvm::Value *a, llvm::Value *b, const Type *type);

private:
  Lowering &lowering_;
};

} // namespace tz
