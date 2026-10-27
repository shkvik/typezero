#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class BinaryLowerer {
public:
  explicit BinaryLowerer(Lowering &lowering);

  llvm::Value *emitBinary(BinaryExpr *bin);
  llvm::Value *emitArithmetic(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs, const Type *type,
                              bool proven = false);
  llvm::Value *emitComparison(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs, const Type *type);
  llvm::Value *emitMixedComparison(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs, const Type *l,
                                   const Type *r);
  llvm::Value *emitIntDivRem(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs, const Type *type,
                             bool proven);

private:
  Lowering &lowering_;
};

}
