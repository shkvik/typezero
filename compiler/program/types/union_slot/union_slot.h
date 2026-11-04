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
  Double,
  Bool,
  Decimal,
  Object,
  Array,
  Function,
  Iterator,
  Json,
  Int8,
  Int16,
  Int32,
  Uint8,
  Uint16,
  Uint32,
  Uint64,
  Param,
  Other,
};

}
