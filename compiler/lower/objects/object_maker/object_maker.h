#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

struct MakerFields {
  const char *cls;
  std::vector<llvm::StringRef> fields;
};

class ObjectMaker {
public:
  explicit ObjectMaker(Lowering &owner);

  llvm::Function *maker(const Type *type, llvm::ArrayRef<MakerFields> orders);
  llvm::Value *atomOf(const Type *type, llvm::Value *text);

private:
  static const std::vector<llvm::StringRef> &orderOf(const Type *type,
                                                     llvm::ArrayRef<MakerFields> orders);
  static bool isNestedPart(const Type *type);
  static std::vector<const Type *> literalsOf(const Type *type);

  llvm::StructType *parts(const Type *type, llvm::ArrayRef<MakerFields> orders);

  Lowering &lowering;
  std::map<const Type *, llvm::Function *> makers;
};

}
