#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ReflectDispatchLowerer {
public:
  explicit ReflectDispatchLowerer(Lowering &lowering);

  llvm::Constant *reflectDispatchInfo(const ClassDecl *cls,
                                      llvm::ArrayRef<const FunctionDecl *> variants,
                                      const Type *&type);
  llvm::Function *invokerFor(const Type *fnType);

private:
  Lowering &lowering_;
  std::map<std::pair<const ClassDecl *, std::string>, llvm::Constant *> reflectDispatchers_;
  llvm::DenseMap<const Type *, llvm::Function *> invokers_;
};

}
