#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

namespace tz {

class DictCheckHelpers {
public:
  static std::string dictText(const Expr *expr, const Type *type);
};

}
