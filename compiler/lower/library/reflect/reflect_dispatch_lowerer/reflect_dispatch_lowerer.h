#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ReflectDispatchLowerer {
public:
  explicit ReflectDispatchLowerer(Lowering &owner);

  llvm::Constant *reflectDispatchInfo(const ClassDecl *cls,
                                      llvm::ArrayRef<const FunctionDecl *> variants,
                                      const Type *&type);
  llvm::Function *invokerFor(const Type *fnType);

private:
  static std::string className(const ClassDecl *cls);

  Lowering &lowering;
  std::map<std::pair<const ClassDecl *, std::string>, llvm::Constant *> reflectDispatchers;
  llvm::DenseMap<const Type *, llvm::Function *> invokers;
};

}
