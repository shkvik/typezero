#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// The calls of node:fs, node:path and node:os, and the functions by which the runtime
// makes objects.
class BuiltinModuleLowerer {
public:
  explicit BuiltinModuleLowerer(Lowering &lowering);

  // The built-in modules: node:path, node:os, node:fs and tz:io
  // (codegen_node.cpp).
  llvm::Value *emitNodeBuiltin(CallExpr *call);
  // `ptr (ptr parts)`: makes an object of `type` from the struct of its
  // fields the runtime fills (TzMaker in runtime/core/tzrt/tzrt.h), taking them
  // over.
  llvm::Function *objectMaker(const Type *type);
  // The struct of the parts of `type` that its maker reads.
  llvm::StructType *makerParts(const Type *type);
  // The maker of SystemError, which the functions of node:fs throw.
  llvm::Function *systemErrorMaker();

private:
  Lowering &lowering_;
  std::map<const Type *, llvm::Function *> objectMakers_;
};

} // namespace tz
