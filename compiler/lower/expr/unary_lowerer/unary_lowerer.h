#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class UnaryLowerer {
public:
  explicit UnaryLowerer(Lowering &owner);

  llvm::Value *emitUnary(UnaryExpr *unary);
  llvm::Value *emitTypeOf(Expr *operand);
  llvm::Value *emitEnumName(IndexExpr *index);

private:
  static std::u16string ascii16(llvm::StringRef text);
  Lowering &lowering;
};

}
