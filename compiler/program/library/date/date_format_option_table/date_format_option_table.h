#pragma once

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class DateFormatOption {
  LOCALE_MATCHER,
  CALENDAR,
  NUMBERING_SYSTEM,
  HOUR_CYCLE,
  TIME_ZONE,
  WEEKDAY,
  ERA,
  YEAR,
  MONTH,
  DAY,
  DAY_PERIOD,
  HOUR,
  MINUTE,
  SECOND,
  TIME_ZONE_NAME,
  FORMAT_MATCHER,
  DATE_STYLE,
  TIME_STYLE,
  HOUR12,
  FRACTIONAL_SECOND_DIGITS,
};

struct DateFormatOptionInfo {
  llvm::StringLiteral name;
  DateFormatOption value;
};

class DateFormatOptionTable {
public:
  static std::optional<DateFormatOption> find(llvm::StringRef name);
};

}
