#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// The functions of Math.
class MathLowerer {
public:
  explicit MathLowerer(Lowering &lowering);

  // Math, Number, parseInt, String.fromCharCode... (codegen_builtins.cpp)
  llvm::Value *emitMathCall(CallExpr *call);
  // Passes `values` to the runtime as an array on the stack.
  llvm::Value *spillArray(llvm::ArrayRef<llvm::Value *> values, llvm::Type *element);
  // A runtime function of doubles: tz_math_sin and the like.
  llvm::FunctionCallee mathFunction(llvm::StringRef name, unsigned params);

private:
  Lowering &lowering_;
};

} // namespace tz
