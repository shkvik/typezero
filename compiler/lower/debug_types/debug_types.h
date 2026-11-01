#pragma once

#include "compiler/lower/lowering/lowering.h"
#include "llvm/IR/DIBuilder.h"

namespace tz {

class DebugTypes {
public:
  DebugTypes(Lowering &lowering, llvm::DIBuilder &builder, llvm::DICompileUnit *unit);

  llvm::DIType *of(const Type *type);
  llvm::DIType *referenceTo(llvm::DIType *type);
  void finish();
  bool usesAtoms() const { return atom_ != nullptr; }

private:
  llvm::DIType *make(const Type *type);
  llvm::DIType *basic(llvm::StringRef name, unsigned bits, unsigned encoding);
  llvm::DIType *pointer(llvm::DIType *to);
  llvm::DIType *voidPointer();
  llvm::DICompositeType *
  structure(llvm::StringRef name, llvm::StructType *layout,
            llvm::ArrayRef<std::pair<llvm::StringRef, llvm::DIType *>> fields,
            llvm::DIType *base = nullptr, llvm::ArrayRef<unsigned> order = {});
  llvm::DIType *header();
  llvm::DIType *string();
  llvm::DIType *array(const Type *type);
  llvm::DIType *optional(const Type *type);
  llvm::DIType *function();
  llvm::DIType *object(const Type *type);
  llvm::DIType *result(const Type *type);
  llvm::DIType *dict(const Type *type);
  llvm::DIType *tagged(const Type *type);
  llvm::DIType *atom();
  llvm::DIType *opaque(const Type *type);
  std::string unique(llvm::StringRef name, const void *owner);
  void completeObject(const Type *type, llvm::DICompositeType *temporary);

  Lowering &lowering_;
  llvm::DIBuilder &builder_;
  llvm::DICompileUnit *unit_;
  llvm::DenseMap<const Type *, llvm::DIType *> types_;
  std::vector<std::pair<const Type *, llvm::DICompositeType *>> pending_;
  llvm::StringMap<const void *> names_;
  llvm::DIType *header_ = nullptr;
  llvm::DIType *string_ = nullptr;
  llvm::DIType *function_ = nullptr;
  llvm::DIType *atom_ = nullptr;
  llvm::DIType *voidPointer_ = nullptr;
};

}
