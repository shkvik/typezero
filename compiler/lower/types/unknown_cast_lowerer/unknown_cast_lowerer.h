#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class UnknownCastLowerer {
public:
  explicit UnknownCastLowerer(Lowering &lowering);

  llvm::Value *emitUnknownAs(llvm::Value *u, const Type *to, llvm::BasicBlock *fail);
  std::vector<const ClassDecl *> classesOfType(const Type *type);
  llvm::Value *classIdIn(llvm::Value *id, llvm::ArrayRef<const ClassDecl *> classes);
  llvm::Value *emitUnknownFit(llvm::Value *u, const Type *to);

private:
  Lowering &lowering_;
};

}
