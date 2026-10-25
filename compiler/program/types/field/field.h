#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <string>
#include <vector>

namespace tz {

class Type;

// A field of an object type. An optional field (`x?: T`) has the type
// `T | undefined`: a field that is not there and one that holds undefined are
// the same thing, so `x: T | undefined` declares the same field.
struct Field {
  std::string name;
  const Type *type = nullptr;
  bool readonly = false;
};

} // namespace tz
