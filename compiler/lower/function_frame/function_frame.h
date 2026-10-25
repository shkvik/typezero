#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// The function being built: its blocks, its scopes and their cleanups, its stack slots.
class FunctionFrame {
public:
  explicit FunctionFrame(Lowering &lowering);

  // Blocks
  llvm::BasicBlock *newBlock(const llvm::Twine &name);
  // Continues emission in `block`, which must not be reachable by falling through.
  void switchTo(llvm::BasicBlock *block);
  // Falls through into `block` (if the current block is still open) and continues there.
  void emitBlock(llvm::BasicBlock *block);
  bool blockIsOpen();
  void branchTo(llvm::BasicBlock *block);
  // After a jump: code that follows lands in a block nothing branches to.
  void startDeadBlock();
  void pushScope();
  void popScope();
  // Releases the variables of scopes_[depth..] without leaving them, before a
  // jump out of them (break, continue, return).
  void emitCleanups(size_t depth);
  void emitScopeCleanup(const Lowering::Scope &scope);
  // A stack slot for a local variable or parameter; managed slots start empty
  // and are registered for release at the end of the current scope.
  llvm::Value *createLocal(const VarSymbol *var, llvm::StringRef name);
  llvm::Value *createManagedSlot(const Type *type, const llvm::Twine &name);
  // A slot for a pointer to an object of a closure, released with the scope.
  llvm::Value *createObjectSlot(const llvm::Twine &name);
  // Writes a value to a fresh stack slot, for runtime functions that take
  // values of any type by address.
  llvm::Value *spill(llvm::Value *value, const llvm::Twine &name);
  llvm::Value *addressOf(const VarSymbol *var);
  llvm::AllocaInst *createStackSlot(llvm::Type *type, const llvm::Twine &name);

private:
  Lowering &lowering_;
};

} // namespace tz
