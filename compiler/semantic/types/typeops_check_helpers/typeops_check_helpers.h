#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MemoryBuffer.h"

namespace tz {

class TypeopsCheckHelpers {
public:
  static std::vector<const Type *> distributedMembers(const Type *type);
};

}
