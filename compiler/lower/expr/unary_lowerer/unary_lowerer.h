#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Unary operators, !, typeof and the names of enums.
class UnaryLowerer {
public:
  explicit UnaryLowerer(CodeGen &codegen);

  llvm::Value *emitUnary(UnaryExpr *unary);
  llvm::Value *emitNonNull(UnaryExpr *unary);
  llvm::Value *emitTypeOf(Expr *operand);
  llvm::Value *emitEnumName(IndexExpr *index);

private:
  CodeGen &codegen_;
};

} // namespace tz
