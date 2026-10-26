#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

struct MakerFields {
  const char *cls;
  std::vector<llvm::StringRef> fields;
};

class ObjectMaker {
public:
  explicit ObjectMaker(Lowering &lowering);

  llvm::Function *maker(const Type *type, llvm::ArrayRef<MakerFields> orders);

private:
  llvm::StructType *parts(const Type *type, llvm::ArrayRef<MakerFields> orders);

  Lowering &lowering_;
  std::map<const Type *, llvm::Function *> makers_;
};

}
