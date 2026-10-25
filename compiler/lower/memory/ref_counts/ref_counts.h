#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// Reference counts: retain and release, and operands borrowed or owned.
class RefCounts {
public:
  explicit RefCounts(Lowering &lowering);

  // Reference counting of any managed value.
  void retainValue(llvm::Value *value, const Type *type);
  void releaseValue(llvm::Value *value, const Type *type);
  Lowering::Operand emitOperand(Expr *expr, bool canBorrow);
  // A variable, or a field read through fields from one, without counting
  // its reference; null for anything else.
  llvm::Value *emitBorrowed(Expr *expr);
  void dropOperand(const Lowering::Operand &operand);
  // Strings
  void retainString(llvm::Value *str);
  void releaseString(llvm::Value *str);

private:
  Lowering &lowering_;
};

} // namespace tz
