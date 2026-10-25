#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// The layouts of objects, their descriptors and their drops.
class ObjectLayouts {
public:
  explicit ObjectLayouts(CodeGen &codegen);

  const CodeGen::ObjectLayout &objectLayout(const Type *type);
  // The constant that describes objects of `type` created with the fields in
  // `keys` order (positions in Type::fields()) to the runtime: the drop
  // function, and the names, offsets and types of the fields in that order,
  // for printing.
  llvm::Constant *layoutDescriptor(const Type *type, llvm::ArrayRef<unsigned> keys);
  // The same with the TZ_LAYOUT_* flags of the descriptor given.
  llvm::Constant *layoutDescriptor(const Type *type, llvm::ArrayRef<unsigned> keys, uint32_t flags);
  void emitObjectDrop(const Type *type, CodeGen::ObjectLayout &layout);
  // The address of field `name` of `object`, which code knows as `type`;
  // with `exact`, the object is of that type and of no other.
  llvm::Value *fieldAddress(llvm::Value *object, const Type *type, llvm::StringRef name,
                            bool exact = false);
  // A new object of `type` with the descriptor for `keys`; its fields are
  // not set yet.
  llvm::Value *allocObject(const Type *type, llvm::ArrayRef<unsigned> keys);
  // The same as a member of a group, or not (see makesMember).
  llvm::Value *allocObject(const Type *type, llvm::ArrayRef<unsigned> keys, bool member);
  // The position of field `name` in Type::fields().
  static unsigned fieldIndex(const Type *type, llvm::StringRef name);

private:
  CodeGen &codegen_;
  std::map<const Type *, CodeGen::ObjectLayout> objectLayouts_; // stable while more are added
};

} // namespace tz
