#pragma once

#include "compiler/program/types/type/type.h"

namespace tz {

// The slots of unions: which values a union holds apart.

// The slot that values of `type` take in a union.
UnionSlot unionSlot(const Type *type);

// Why values of the members of `type`, a union, cannot be told apart when
// the program runs (`int | double`: both are numbers to typeof), or why one
// cannot be a member; empty if nothing is wrong.
std::string unionProblem(const Type *type);

// Whether a member of the union `type` is a type parameter (generic code).
bool hasParamMember(const Type *type);

// `type` without the members of the union `without` (or `without` itself):
// what a narrowed value is where a check failed. Null if nothing is left.
const Type *unionWithout(const Type *type, const Type *without);

} // namespace tz
