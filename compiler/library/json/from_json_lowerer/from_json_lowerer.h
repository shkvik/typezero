#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class FromJsonLowerer {
public:
  explicit FromJsonLowerer(Lowering &lowering);

  llvm::Value *emitFromJson(const Type *type, llvm::Value *json, llvm::Value *path);
  llvm::Value *emitFromJsonOrSkip(const Type *type, llvm::Value *json, llvm::Value *path);
  llvm::Function *fromJsonFunction(const Type *type);

private:
  Lowering &lowering_;
  llvm::DenseMap<const Type *, llvm::Function *> fromJsonFunctions_;
};

}
