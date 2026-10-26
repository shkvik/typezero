#pragma once

#include "compiler/check/checker/checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

namespace tz {

void collectNames(const Stmt *stmt, llvm::StringSet<> &names, bool inFunction);

void collectNames(const Expr *expr, llvm::StringSet<> &names, bool inFunction);

}
