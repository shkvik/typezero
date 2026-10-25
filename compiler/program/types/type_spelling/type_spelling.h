#pragma once

#include "compiler/program/types/type/type.h"

namespace tz {

// How types are spelled and named: object types, typeof, and the tag field of JSON.

// `{ x: int; y?: string }`: an object type spelled out.
std::string objectTypeSpelling(llvm::ArrayRef<Field> fields);

// What `typeof` gives for a value of `type` ("number", "string"...). For
// `T | undefined` that holds a value, it is the name for T.
const char *typeOfName(const Type *type);

// The field that tells the object types `objects` apart in JSON, for
// JSON.parse<T> of a union of them (docs/narrowing.md, section 3): one that
// each of them has, of a literal type, with another value in each, as
// `kind: "circle"`. Empty if there is none.
std::string jsonTagField(llvm::ArrayRef<const Type *> objects);

} // namespace tz
