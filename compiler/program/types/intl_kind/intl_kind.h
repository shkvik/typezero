#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <string>
#include <vector>

namespace tz {

// The objects of Intl (docs/date.md, docs/intl.md) that the runtime makes:
// each kind is an object type of its own, without fields the program sees.
// Segments is what Intl.Segmenter's segment() gives.
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

} // namespace tz
