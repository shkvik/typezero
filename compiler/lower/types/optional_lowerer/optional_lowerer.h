#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class OptionalLowerer {
public:
  explicit OptionalLowerer(Lowering &owner);

  llvm::Value *makeOptional(llvm::Value *value, llvm::Value *has, const Type *type);
  llvm::Value *undefinedValue(const Type *optionalType);
  llvm::Value *optionalHas(llvm::Value *value, const Type *optionalType);
  llvm::Value *optionalValue(llvm::Value *value, const Type *optionalType);

private:
  static bool isReference(const Type *type);

  Lowering &lowering;
};

}
