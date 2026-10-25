#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Optionals: T | undefined.
class OptionalLowerer {
public:
  explicit OptionalLowerer(CodeGen &codegen);

  // `T | undefined`: whether it holds a value, and the value (zero when it
  // does not).
  llvm::Value *makeOptional(llvm::Value *value, llvm::Value *has, const Type *type);
  llvm::Value *undefinedValue(const Type *optionalType);
  llvm::Value *optionalHas(llvm::Value *value, const Type *optionalType);
  llvm::Value *optionalValue(llvm::Value *value, const Type *optionalType);

private:
  CodeGen &codegen_;
};

} // namespace tz
