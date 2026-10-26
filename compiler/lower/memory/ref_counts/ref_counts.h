#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class RefCounts {
public:
  explicit RefCounts(Lowering &lowering);

  void retainValue(llvm::Value *value, const Type *type);
  void releaseValue(llvm::Value *value, const Type *type);
  Lowering::Operand emitOperand(Expr *expr, bool canBorrow);
  llvm::Value *emitBorrowed(Expr *expr);
  void dropOperand(const Lowering::Operand &operand);
  void retainString(llvm::Value *str);
  void releaseString(llvm::Value *str);

private:
  Lowering &lowering_;
};

}
