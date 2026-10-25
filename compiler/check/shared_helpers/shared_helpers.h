#pragma once

#include "compiler/check/checker/checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

namespace tz {

// Functions that several classes of the check use.

// `text` without parentheses around all of it, which `${...}` makes
// unnecessary. Text with quotes keeps them: a parenthesis in a string would
// confuse the count.
llvm::StringRef withoutParens(llvm::StringRef text);

// The type of a literal as written: an int for `10` (see sema.cpp).
const Type *literalType(const Expr *value);

} // namespace tz
