#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

namespace tz {

class DictCheckHelpers {
public:
  static std::string dictText(const Expr *expr, const Type *type);

private:
  static const char *dictPlaceholder(const Type *type);
};

}
