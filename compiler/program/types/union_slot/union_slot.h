#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <string>
#include <vector>

namespace tz {

enum class UnionSlot : uint8_t {
  None = 0,
  String,
  Atom,
  Int,
  Long,
  Double,
  Bool,
  Decimal,
  Object,
  Array,
  Function,
  Iterator,
  Param,
  Other,
};

}
