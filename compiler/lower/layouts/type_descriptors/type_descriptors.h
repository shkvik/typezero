#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class TypeDescriptors {
public:
  explicit TypeDescriptors(Lowering &lowering);

  llvm::Constant *typeDescriptor(const Type *type);
  static uint32_t descriptorKind(const Type *type);

private:
  Lowering &lowering_;
};

}
