#pragma once

#include "compiler/program/types/type/type.h"

namespace tz {

// Argument lists of types: spreads into tuples, and the parameters they flatten to.

// `[parts...]` with those marked in `spread` spread into it, `[int, ...A]`:
// a list of types (isListParam) stays an element, spread there; a tuple
// gives its elements, and an array, last, the rest of an argument list
// (Type::argumentList). Null with `why` set if that cannot be.
const Type *spreadTuple(llvm::ArrayRef<const Type *> parts, const std::vector<bool> &spread,
                        std::string &why);

// The parameters `params` of a function, whose last one is `...args: L` of
// an argument list L (a tuple, see Type::argumentList), with the types of L
// in its place: `(int, ...[string, bool?])` is `(int, string, bool?)`.
// False if there is no such last parameter; `minArgs` and `hasRest` are
// those of the result.
bool flattenArguments(llvm::ArrayRef<const Type *> params, size_t &minArgs, bool &hasRest,
                      std::vector<const Type *> &flat);

} // namespace tz
