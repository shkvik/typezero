#pragma once

#include "compiler/check/sema/sema.h"
#include "llvm/Support/ConvertUTF.h"

namespace tz {

// Functions that several classes made from sema_union.cpp use.

// The literal type of a literal as written (before it is checked or after),
// looking through the conversions of a checked one.
const Type *writtenLiteral(const Expr *expr);

} // namespace tz
