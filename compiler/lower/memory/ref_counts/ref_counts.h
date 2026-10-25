#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Reference counts: retain and release, and operands borrowed or owned.
class RefCounts {
public:
  explicit RefCounts(CodeGen &codegen);

  // Reference counting of any managed value.
  void retainValue(llvm::Value *value, const Type *type);
  void releaseValue(llvm::Value *value, const Type *type);
  CodeGen::Operand emitOperand(Expr *expr, bool canBorrow);
  // A variable, or a field read through fields from one, without counting
  // its reference; null for anything else.
  llvm::Value *emitBorrowed(Expr *expr);
  void dropOperand(const CodeGen::Operand &operand);
  // Strings
  void retainString(llvm::Value *str);
  void releaseString(llvm::Value *str);

private:
  CodeGen &codegen_;
};

} // namespace tz
