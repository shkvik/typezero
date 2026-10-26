#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

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

}
