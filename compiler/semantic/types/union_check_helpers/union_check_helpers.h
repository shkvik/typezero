#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"
#include "llvm/Support/ConvertUTF.h"

namespace tz {

class UnionCheckHelpers {
public:
  static const Type *writtenLiteral(const Expr *expr);
};

}
