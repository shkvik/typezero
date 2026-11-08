#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>

namespace tz {

class ConditionalCheckHelpers {
public:
  static void distribute(const Type *type, std::vector<const Type *> &out);
};

}
