#pragma once

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <optional>
#include <vector>

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
  std::vector<llvm::StringLiteral> values;
};

class DateFormatOptionTable {
public:
  static std::optional<DateFormatOption> find(llvm::StringRef name);
  static llvm::ArrayRef<DateFormatOptionInfo> getOptions();
  static llvm::ArrayRef<llvm::StringLiteral> getValues(llvm::StringRef name);
};

}
