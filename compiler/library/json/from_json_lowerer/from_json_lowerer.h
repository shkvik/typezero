#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// The functions of JSON.parse<T>.
class FromJsonLowerer {
public:
  explicit FromJsonLowerer(Lowering &lowering);

  // JSON.parse<T> and fromJson<T>: a value of `type` built from the borrowed
  // Json `json` (null for undefined) that `path` says where it is, for the
  // error. Objects and arrays are built by a function of their own.
  llvm::Value *emitFromJson(const Type *type, llvm::Value *json, llvm::Value *path);
  llvm::Function *fromJsonFunction(const Type *type);

private:
  Lowering &lowering_;
  llvm::DenseMap<const Type *, llvm::Function *> fromJsonFunctions_;
};

} // namespace tz
