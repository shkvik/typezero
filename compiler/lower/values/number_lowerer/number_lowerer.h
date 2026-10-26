#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class NumberLowerer {
public:
  explicit NumberLowerer(Lowering &lowering);

  llvm::Value *emitNumberBuiltin(CallExpr *call);
  llvm::Value *emitStringToNumber(llvm::Value *str);
  llvm::Value *emitCheckedInteger(llvm::Value *value, const Type *to);

private:
  Lowering &lowering_;
};

}
