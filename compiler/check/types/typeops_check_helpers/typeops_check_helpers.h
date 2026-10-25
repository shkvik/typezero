#pragma once

#include "compiler/check/sema/sema.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/MemoryBuffer.h"

namespace tz {

// Functions that several classes made from sema_typeops.cpp use.

// The members Exclude and Extract go over, as a conditional type does:
// those of a union, undefined of an optional, false and true of boolean.
std::vector<const Type *> distributedMembers(const Type *type);

} // namespace tz
