#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Generators as coroutines: their layout, next and return.
class GeneratorLowerer {
public:
  explicit GeneratorLowerer(CodeGen &codegen);

  // Coroutines.
  llvm::Constant *iteratorLayout(unsigned flags);
  void emitGenerator(FunctionDecl *fn, llvm::Function *function);
  void emitPendingIterators();
  llvm::Function *iteratorFunction(Builtin builtin, const Type *source, const Type *operand,
                                   const Type *result);
  // `void tz.iter.close(ptr iterator)`: return() of JavaScript.
  llvm::Function *iterCloseFunction();

private:
  CodeGen &codegen_;
  std::map<std::tuple<Builtin, const Type *, const Type *, const Type *>, llvm::Function *>
      iteratorFunctions_;
  llvm::Function *iterDropFn_ = nullptr;
  llvm::Function *iterCloseFn_ = nullptr;
  std::map<unsigned, llvm::Constant *> iteratorLayouts_;
};

} // namespace tz
