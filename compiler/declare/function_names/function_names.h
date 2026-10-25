#pragma once

#include "compiler/check/sema/sema.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

namespace tz {

// The names statements mention, to know which globals functions may use.

// The names a statement mentions, to know which globals functions may use.
// With `inFunction` unset, only the names inside the functions it contains
// count.
void collectNames(const Stmt *stmt, llvm::StringSet<> &names, bool inFunction);

void collectNames(const Expr *expr, llvm::StringSet<> &names, bool inFunction);

} // namespace tz
