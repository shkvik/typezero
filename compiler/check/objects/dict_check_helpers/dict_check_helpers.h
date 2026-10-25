#pragma once

#include "compiler/check/sema/sema.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

namespace tz {

// Functions that several classes made from sema_dict.cpp use.

// The name a dictionary goes by in messages: `r`, `m`, `s`, or the variable
// that holds it.
std::string dictText(const Expr *expr, const Type *type);

} // namespace tz
