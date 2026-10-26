#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class MathLowerer {
public:
  explicit MathLowerer(Lowering &lowering);

  llvm::Value *emitMathCall(CallExpr *call);
  llvm::Value *spillArray(llvm::ArrayRef<llvm::Value *> values, llvm::Type *element);
  llvm::FunctionCallee mathFunction(llvm::StringRef name, unsigned params);

private:
  Lowering &lowering_;
};

}
