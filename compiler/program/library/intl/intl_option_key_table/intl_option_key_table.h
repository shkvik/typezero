#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class IntlOptionKey {
  LOCALE_MATCHER,
  NUMBERING_SYSTEM,
  STYLE,
  CURRENCY,
  CURRENCY_DISPLAY,
  CURRENCY_SIGN,
  UNIT,
  UNIT_DISPLAY,
  USE_GROUPING,
  NOTATION,
  COMPACT_DISPLAY,
  SIGN_DISPLAY,
  MINIMUM_INTEGER_DIGITS,
  MINIMUM_FRACTION_DIGITS,
  MAXIMUM_FRACTION_DIGITS,
  MINIMUM_SIGNIFICANT_DIGITS,
  MAXIMUM_SIGNIFICANT_DIGITS,
  ROUNDING_INCREMENT,
  ROUNDING_MODE,
  ROUNDING_PRIORITY,
  TRAILING_ZERO_DISPLAY,
  TYPE,
  USAGE,
  SENSITIVITY,
  IGNORE_PUNCTUATION,
  COLLATION,
  NUMERIC,
  CASE_FIRST,
  FALLBACK,
  LANGUAGE_DISPLAY,
  GRANULARITY,
};

struct IntlOptionKeyInfo {
  llvm::StringLiteral name;
  IntlOptionKey value;
};

class IntlOptionKeyTable {
public:
  static std::optional<IntlOptionKey> find(llvm::StringRef name);
  static llvm::StringRef getName(IntlOptionKey value);
};

}
