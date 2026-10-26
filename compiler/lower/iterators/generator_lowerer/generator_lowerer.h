#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class GeneratorLowerer {
public:
  explicit GeneratorLowerer(Lowering &lowering);

  llvm::Constant *iteratorLayout(unsigned flags);
  void emitGenerator(FunctionDecl *fn, llvm::Function *function);
  void emitPendingIterators();
  llvm::Function *iteratorFunction(Builtin builtin, const Type *source, const Type *operand,
                                   const Type *result);
  llvm::Function *iterCloseFunction();

private:
  Lowering &lowering_;
  std::map<std::tuple<Builtin, const Type *, const Type *, const Type *>, llvm::Function *>
      iteratorFunctions_;
  llvm::Function *iterDropFn_ = nullptr;
  llvm::Function *iterCloseFn_ = nullptr;
  std::map<unsigned, llvm::Constant *> iteratorLayouts_;
};

}
