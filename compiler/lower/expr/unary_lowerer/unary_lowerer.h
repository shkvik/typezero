#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// Unary operators, !, typeof and the names of enums.
class UnaryLowerer {
public:
  explicit UnaryLowerer(Lowering &lowering);

  llvm::Value *emitUnary(UnaryExpr *unary);
  llvm::Value *emitNonNull(UnaryExpr *unary);
  llvm::Value *emitTypeOf(Expr *operand);
  llvm::Value *emitEnumName(IndexExpr *index);

private:
  Lowering &lowering_;
};

} // namespace tz
