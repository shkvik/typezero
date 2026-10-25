#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// The environments of closures and the boxes of shared variables.
class EnvLowerer {
public:
  explicit EnvLowerer(CodeGen &codegen);

  const CodeGen::EnvLayout &envLayout(const FunctionDecl *fn);
  llvm::StructType *boxType(const Type *type);
  // A new box for a variable of `type`, holding its zero value; a member of
  // a group when `member` is, or the variable's box is cyclic.
  llvm::Value *newBox(const Type *type, bool member = false);
  llvm::Value *boxPointer(const VarSymbol *var);
  // A new environment with the current values of `layout`'s variables. On
  // the stack, it borrows them and is not counted.
  llvm::Value *createEnv(const CodeGen::EnvLayout &layout, bool onStack, bool member = true);
  // The value of a variable, owned or borrowed: a function's own value when
  // the variable names the function being emitted or one of its group.
  llvm::Value *emitVarValue(const VarSymbol *var, bool borrow);
  void emitGroupCreation(NestedGroup *group);
  // The addresses of the variables a closure captured, in its environment.
  void bindCaptures(const FunctionDecl *fn);

private:
  CodeGen &codegen_;
  llvm::DenseMap<const Type *, llvm::StructType *> boxTypes_;
};

} // namespace tz
