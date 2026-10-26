#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class FunctionFrame {
public:
  explicit FunctionFrame(Lowering &lowering);

  llvm::BasicBlock *newBlock(const llvm::Twine &name);
  void switchTo(llvm::BasicBlock *block);
  void emitBlock(llvm::BasicBlock *block);
  bool blockIsOpen();
  void branchTo(llvm::BasicBlock *block);
  void startDeadBlock();
  void pushScope();
  void popScope();
  void emitCleanups(size_t depth);
  void emitScopeCleanup(const Lowering::Scope &scope);
  llvm::Value *createLocal(const VarSymbol *var, llvm::StringRef name);
  llvm::Value *createManagedSlot(const Type *type, const llvm::Twine &name);
  llvm::Value *createObjectSlot(const llvm::Twine &name);
  llvm::Value *spill(llvm::Value *value, const llvm::Twine &name);
  llvm::Value *addressOf(const VarSymbol *var);
  llvm::AllocaInst *createStackSlot(llvm::Type *type, const llvm::Twine &name);

private:
  Lowering &lowering_;
};

}
