#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// The functions of Math.
class MathLowerer {
public:
  explicit MathLowerer(CodeGen &codegen);

  // Math, Number, parseInt, String.fromCharCode... (codegen_builtins.cpp)
  llvm::Value *emitMathCall(CallExpr *call);
  // Passes `values` to the runtime as an array on the stack.
  llvm::Value *spillArray(llvm::ArrayRef<llvm::Value *> values, llvm::Type *element);
  // A runtime function of doubles: tz_math_sin and the like.
  llvm::FunctionCallee mathFunction(llvm::StringRef name, unsigned params);

private:
  CodeGen &codegen_;
};

} // namespace tz
