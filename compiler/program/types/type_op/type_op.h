#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <string>
#include <vector>

namespace tz {

// Computations over types of generic code (docs/generics.md, section 10),
// which wait until a copy gives their operands types: `keyof T`, `T[K]`,
// `Exclude<T, U>`, `Extract<T, U>`, a mapped type `{ [K in keyof T]: ... }`,
// `ReturnType<F>`, `Parameters<F>`, `NonNullable<T>`, a conditional type
// `T extends U ? A : B`, a template literal type `` `on${K}` ``, the
// intrinsic `Uppercase<S>`, `Lowercase<S>`, `Capitalize<S>` and
// `Uncapitalize<S>`, and a tuple with such a computation spread into it,
// `[...Tuple<A>, ...Tuple<B>]` (its operands are pairs of whether the
// element is spread, as a boolean literal type, and the element).
enum class TypeOp : uint8_t {
  None,
  KeyOf,
  Index,
  Exclude,
  Extract,
  Mapped,
  ReturnType,
  Parameters,
  NonNullable,
  Conditional,
  Template,
  Uppercase,
  Lowercase,
  Capitalize,
  Uncapitalize,
  Spread,
};

} // namespace tz
