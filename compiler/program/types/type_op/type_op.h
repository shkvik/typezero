#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <string>
#include <vector>

namespace tz {

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

}
