#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// The descriptors of types the runtime reads: how to release and print a value.
class TypeDescriptors {
public:
  explicit TypeDescriptors(CodeGen &codegen);

  // Arrays (codegen_array.cpp)
  llvm::Constant *typeDescriptor(const Type *type);

private:
  CodeGen &codegen_;
};

} // namespace tz
