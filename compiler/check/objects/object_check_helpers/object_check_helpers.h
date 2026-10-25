#pragma once

#include "compiler/check/sema/sema.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

namespace tz {

// Functions that several classes made from sema_object.cpp use.

// The expression a conversion that narrows a variable or a field converts:
// to the value of a `T | undefined`, or a variable to the class `instanceof`
// showed (`e.stdout` of `e` that is an ExecSyncError is a field of `e`).
const Expr *unnarrowed(const Expr *expr);

} // namespace tz
