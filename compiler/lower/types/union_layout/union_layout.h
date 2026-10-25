#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// The representation of unions: tags, slots and descriptors.
class UnionLayout {
public:
  explicit UnionLayout(CodeGen &codegen);

  // Unions and literal types (codegen_union.cpp): how their values lie is
  // described there. A string literal type or a union of them is an atom.
  static bool isAtomType(const Type *type);
  static bool isObjectUnion(const Type *type);
  // The type whose values a value of `type` is: the base of a number or
  // boolean literal type (or a union of them, `0 | 1`); `type` otherwise.
  static const Type *storedType(const Type *type);
  // Whether `T | undefined` is T itself, zero for undefined: a reference,
  // an atom, a tagged union.
  static bool zeroIsUndefined(const Type *inner);
  llvm::Type *lowerUnion(const Type *type);
  // The constant table of the strings of the atoms, and the text of one.
  llvm::GlobalVariable *atomTable();
  llvm::Value *atomString(llvm::Value *atom);
  llvm::Constant *atomDescriptor();
  llvm::Constant *unionDescriptor(const Type *type);
  // Tagged unions: the tag; a value from the part of a slot; the part.
  llvm::Value *unionTag(llvm::Value *value);
  llvm::Value *makeTagged(const Type *unionType, UnionSlot slot, llvm::Value *part);
  llvm::Value *taggedPart(llvm::Value *value, const Type *unionType, UnionSlot slot);
  // Branches on the tag of `value`: `each` emits the code of a slot, the
  // result for it; a tag of none of `slots` gives `otherwise`. Returns the
  // result of the one that ran (null without `resultType`).
  llvm::Value *forEachSlot(llvm::Value *value, llvm::ArrayRef<UnionSlot> slots,
                           llvm::function_ref<llvm::Value *(UnionSlot)> each,
                           llvm::Type *resultType, llvm::Value *otherwise);
  void countTagged(llvm::Value *value, const Type *type, bool retain);

private:
  CodeGen &codegen_;
  llvm::GlobalVariable *atomTable_ = nullptr;
  llvm::Constant *atomDescriptor_ = nullptr;
};

} // namespace tz
