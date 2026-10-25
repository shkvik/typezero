#pragma once

#include "compiler/check/checker/checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>

namespace tz {

// Functions that several classes made from sema_conditional.cpp use.

// The members a conditional type goes over one by one when the type it
// checks is a type parameter: those of a union, undefined of an optional, and
// false and true of boolean, as in TypeScript.
void distribute(const Type *type, std::vector<const Type *> &out);

} // namespace tz
