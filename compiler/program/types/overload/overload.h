#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <string>
#include <vector>

namespace tz {

class Type;

// A method of an interface with several signatures (docs/overloads.md,
// section 4), `add(v: Vec): Vec; add(k: double): Vec`: no field holds it,
// so only a class implements it, each signature with a variant of its own.
struct Overload {
  std::string name;
  std::vector<const Type *> signatures; // function types
};

} // namespace tz
