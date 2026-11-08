#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

namespace tz {

class ObjectCheckHelpers {
public:
  static const Expr *unnarrowed(const Expr *expr);
};

}
