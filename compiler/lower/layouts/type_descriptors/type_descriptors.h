#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// The descriptors of types the runtime reads: how to release and print a value.
class TypeDescriptors {
public:
  explicit TypeDescriptors(Lowering &lowering);

  // Arrays (codegen_array.cpp)
  llvm::Constant *typeDescriptor(const Type *type);

private:
  Lowering &lowering_;
};

} // namespace tz
