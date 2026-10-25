#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// The tables through which reflection calls methods.
class ReflectDispatchLowerer {
public:
  explicit ReflectDispatchLowerer(Lowering &lowering);

  // The function value Reflect.get gives of a method of several variants:
  // of `type`, `(this: unknown, ...args: unknown[]) => unknown`, which chooses
  // the variant by its arguments when it is called. The info of the value.
  llvm::Constant *reflectDispatchInfo(const ClassDecl *cls,
                                      llvm::ArrayRef<const FunctionDecl *> variants,
                                      const Type *&type);
  llvm::Function *invokerFor(const Type *fnType);

private:
  Lowering &lowering_;
  std::map<std::pair<const ClassDecl *, std::string>, llvm::Constant *> reflectDispatchers_;
  llvm::DenseMap<const Type *, llvm::Function *> invokers_;
};

} // namespace tz
