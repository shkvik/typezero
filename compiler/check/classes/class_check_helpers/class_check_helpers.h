#pragma once

#include "compiler/check/checker/checker.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

// Functions that several classes made from sema_class.cpp use.

// `add(v: Vec, k?: double)`, a variant in messages.
std::string variantText(const FunctionDecl *fn);

} // namespace tz
