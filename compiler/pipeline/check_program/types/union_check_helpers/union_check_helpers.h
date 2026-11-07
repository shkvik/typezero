#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "llvm/Support/ConvertUTF.h"

namespace tz {

class UnionCheckHelpers {
public:
  static const Type *writtenLiteral(const Expr *expr);
};

}
