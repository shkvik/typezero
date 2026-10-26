#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ObjectLayouts {
public:
  explicit ObjectLayouts(Lowering &lowering);

  const Lowering::ObjectLayout &objectLayout(const Type *type);
  llvm::Constant *layoutDescriptor(const Type *type, llvm::ArrayRef<unsigned> keys);
  llvm::Constant *layoutDescriptor(const Type *type, llvm::ArrayRef<unsigned> keys, uint32_t flags);
  void emitObjectDrop(const Type *type, Lowering::ObjectLayout &layout);
  llvm::Value *fieldAddress(llvm::Value *object, const Type *type, llvm::StringRef name,
                            bool exact = false);
  llvm::Value *allocObject(const Type *type, llvm::ArrayRef<unsigned> keys);
  llvm::Value *allocObject(const Type *type, llvm::ArrayRef<unsigned> keys, bool member);
  static unsigned fieldIndex(const Type *type, llvm::StringRef name);

private:
  Lowering &lowering_;
  std::map<const Type *, Lowering::ObjectLayout> objectLayouts_;
};

}
