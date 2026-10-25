#pragma once

#include "compiler/lower/codegen/codegen.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

// Functions that several classes made from codegen_iter.cpp use.

// Whether `stmt` has a `try`, outside the functions in it.
bool hasTry(const Stmt *stmt);

} // namespace tz
