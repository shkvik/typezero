#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class EnvLowerer {
public:
  explicit EnvLowerer(Lowering &lowering);

  const Lowering::EnvLayout &envLayout(const FunctionDecl *fn);
  llvm::StructType *boxType(const Type *type);
  llvm::Value *newBox(const Type *type, bool member = false);
  llvm::Value *boxPointer(const VarSymbol *var);
  llvm::Value *createEnv(const Lowering::EnvLayout &layout, bool onStack, bool member = true);
  llvm::Value *emitVarValue(const VarSymbol *var, bool borrow);
  void emitGroupCreation(NestedGroup *group);
  void bindCaptures(const FunctionDecl *fn);

private:
  Lowering &lowering_;
  llvm::DenseMap<const Type *, llvm::StructType *> boxTypes_;
};

}
