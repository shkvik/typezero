#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MemoryBuffer.h"

namespace tz {

class TypeopsCheckHelpers {
public:
  static std::vector<const Type *> distributedMembers(const Type *type);
};

}
