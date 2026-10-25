#pragma once

#include "compiler/program/types/type/type.h"

namespace tz {

// Questions about types: how far up a chain, whether they mention a type parameter.

// How many steps up the declared chain of `from` (Type::bases) `to` is: 0
// for the same type, -1 if it is not up the chain.
int subtypeDistance(const Type *from, const Type *to);

// Whether `type` mentions a type parameter anywhere.
bool mentionsParam(const Type *type);

// Whether `type` mentions a type parameter other than `allowed` (the types
// `infer` declares, the key of a mapped type), as a computation that waits
// for a copy does.
bool mentionsParamBesides(const Type *type, llvm::ArrayRef<const Type *> allowed);

} // namespace tz
