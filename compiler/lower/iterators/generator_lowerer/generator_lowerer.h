#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class GeneratorLowerer {
public:
  explicit GeneratorLowerer(Lowering &owner);

  llvm::Constant *iteratorLayout(unsigned flags);
  void emitGenerator(FunctionDecl *fn, llvm::Function *function);
  void emitPendingIterators();
  llvm::Function *iteratorFunction(Builtin builtin, const Type *source, const Type *operand,
                                   const Type *result);

private:
  Lowering &lowering;
  std::map<std::tuple<Builtin, const Type *, const Type *, const Type *>, llvm::Function *>
      iteratorFunctions;
  llvm::Function *iterDropFn = nullptr;
  std::map<unsigned, llvm::Constant *> iteratorLayouts;
};

}
