#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class UnionLayout {
public:
  explicit UnionLayout(Lowering &lowering);

  static bool isAtomType(const Type *type);
  static bool isObjectUnion(const Type *type);
  static const Type *storedType(const Type *type);
  static bool zeroIsUndefined(const Type *inner);
  llvm::Type *lowerUnion(const Type *type);
  llvm::GlobalVariable *atomTable();
  llvm::Value *atomString(llvm::Value *atom);
  llvm::Constant *atomDescriptor();
  llvm::Constant *unionDescriptor(const Type *type);
  llvm::Value *unionTag(llvm::Value *value);
  llvm::Value *makeTagged(const Type *unionType, UnionSlot slot, llvm::Value *part);
  llvm::Value *taggedPart(llvm::Value *value, const Type *unionType, UnionSlot slot);
  llvm::Value *forEachSlot(llvm::Value *value, llvm::ArrayRef<UnionSlot> slots,
                           llvm::function_ref<llvm::Value *(UnionSlot)> each,
                           llvm::Type *resultType, llvm::Value *otherwise);
  void countTagged(llvm::Value *value, const Type *type, bool retain);

private:
  Lowering &lowering_;
  llvm::GlobalVariable *atomTable_ = nullptr;
  llvm::Constant *atomDescriptor_ = nullptr;
};

}
