#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class StoreLowerer {
public:
  explicit StoreLowerer(Lowering &owner);

  void groupStore(llvm::Value *owner, llvm::Value *value, const Type *type);
  void storeBarrier(llvm::Value *owner, llvm::Value *value, const Type *type);
  void publishValue(llvm::Value *value, const Type *type);
  void releaseIn(llvm::Value *owner, llvm::Value *value, const Type *type);
  void takeFrom(llvm::Value *owner, llvm::Value *value, const Type *type);
  void storeIn(llvm::Value *owner, const Type *ownerType, llvm::Value *address, llvm::Value *value,
               const Type *type);
  void storeVar(const VarSymbol *var, llvm::Value *address, llvm::Value *value, bool replacing);
  void emitMemberDrop(const Type *type, Lowering::ObjectLayout &layout);
  void forgetWeakRefs(llvm::Value *object);
  void releaseObjectIn(llvm::Value *owner, llvm::Value *object);

private:
  static llvm::BasicBlock *blockHere(llvm::IRBuilder<> &b, const char *name);
  static llvm::Value *spillHere(llvm::IRBuilder<> &b, llvm::Value *value, const char *name);

  Lowering &lowering;
};

}
