#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <string>
#include <vector>

namespace tz {

enum class IntlKind : uint8_t {
  None,
  DateTimeFormat,
  NumberFormat,
  Collator,
  PluralRules,
  RelativeTimeFormat,
  ListFormat,
  DisplayNames,
  Segmenter,
  Segments,
};

}
