#pragma once

#include "compiler/check/checker/checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

namespace tz {

llvm::StringRef withoutParens(llvm::StringRef text);

const Type *literalType(const Expr *value);

}
