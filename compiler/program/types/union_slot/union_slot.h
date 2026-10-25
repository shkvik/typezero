#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <string>
#include <vector>

namespace tz {

// The parts of a union that lie apart in memory (docs/objects.md, section
// 7). A value of a union whose members take more than one slot holds the
// number of its slot as a tag, the same number for a slot in every union:
// converting between unions keeps the tag. The runtime reads it too (see
// TZ_T_UNION in runtime/core/tzrt/tzrt.h).
enum class UnionSlot : uint8_t {
  None = 0, // undefined, in `A | B | undefined`
  String,
  Atom, // string literals
  Int,
  Long,
  Double,
  Bool,
  Decimal,
  Object, // objects of any types, told apart by their descriptors
  Array,
  Function,
  Iterator,
  Param, // a type parameter, in generic code only
  Other, // Json, Result, void: not part of a union
};

} // namespace tz
