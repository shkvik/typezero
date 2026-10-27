#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class NumberLowerer {
public:
  explicit NumberLowerer(Lowering &lowering);

  llvm::Value *emitNumberBuiltin(CallExpr *call);
  llvm::Value *emitStringToNumber(llvm::Value *str);
  llvm::Value *emitCheckedInteger(llvm::Value *value, const Type *to);
  llvm::Value *toI64(llvm::Value *value, const Type *type);
  llvm::Value *emitCheckedIntCast(llvm::Value *value, const Type *from, const Type *to);
  llvm::Value *intCast(llvm::Value *value, const Type *from, const Type *to);
  llvm::Value *toDouble(llvm::Value *value, const Type *from);
  llvm::Value *fromDouble(llvm::Value *value, const Type *to);

private:
  Lowering &lowering_;
};

}
