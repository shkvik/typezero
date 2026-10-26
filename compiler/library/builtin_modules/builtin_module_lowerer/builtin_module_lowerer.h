#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class BuiltinModuleLowerer {
public:
  explicit BuiltinModuleLowerer(Lowering &lowering);

  llvm::Value *emitNodeBuiltin(CallExpr *call);
  llvm::Function *objectMaker(const Type *type);
  llvm::StructType *makerParts(const Type *type);
  llvm::Function *systemErrorMaker();

private:
  Lowering &lowering_;
  std::map<const Type *, llvm::Function *> objectMakers_;
};

}
