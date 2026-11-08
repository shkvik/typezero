#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

namespace tz {

class SharedHelpers {
public:
  static llvm::StringRef withoutParens(llvm::StringRef text);
  static const Type *literalType(const Expr *value);
};

}
