#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Arithmetic and comparisons.
class BinaryLowerer {
public:
  explicit BinaryLowerer(CodeGen &codegen);

  llvm::Value *emitBinary(BinaryExpr *bin);
  llvm::Value *emitArithmetic(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs, const Type *type);
  llvm::Value *emitComparison(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs, const Type *type);
  llvm::Value *emitIntDivRem(BinaryOp op, llvm::Value *lhs, llvm::Value *rhs);

private:
  CodeGen &codegen_;
};

} // namespace tz
