#include "compiler/program/library/date/date_format_option_table/date_format_option_table.h"

namespace tz {

namespace {

const DateFormatOptionInfo kRows[] = {
    {
        .name = "localeMatcher",
        .value = DateFormatOption::LOCALE_MATCHER,
        .values = {"lookup", "best fit"},
    },
    {
        .name = "calendar",
        .value = DateFormatOption::CALENDAR,
        .values = {},
    },
    {
        .name = "numberingSystem",
        .value = DateFormatOption::NUMBERING_SYSTEM,
        .values = {},
    },
    {
        .name = "hourCycle",
        .value = DateFormatOption::HOUR_CYCLE,
        .values = {"h11", "h12", "h23", "h24"},
    },
    {
        .name = "timeZone",
        .value = DateFormatOption::TIME_ZONE,
        .values = {},
    },
    {
        .name = "weekday",
        .value = DateFormatOption::WEEKDAY,
        .values = {"narrow", "short", "long"},
    },
    {
        .name = "era",
        .value = DateFormatOption::ERA,
        .values = {"narrow", "short", "long"},
    },
    {
        .name = "year",
        .value = DateFormatOption::YEAR,
        .values = {"2-digit", "numeric"},
    },
    {
        .name = "month",
        .value = DateFormatOption::MONTH,
        .values = {"2-digit", "numeric", "narrow", "short", "long"},
    },
    {
        .name = "day",
        .value = DateFormatOption::DAY,
        .values = {"2-digit", "numeric"},
    },
    {
        .name = "dayPeriod",
        .value = DateFormatOption::DAY_PERIOD,
        .values = {"narrow", "short", "long"},
    },
    {
        .name = "hour",
        .value = DateFormatOption::HOUR,
        .values = {"2-digit", "numeric"},
    },
    {
        .name = "minute",
        .value = DateFormatOption::MINUTE,
        .values = {"2-digit", "numeric"},
    },
    {
        .name = "second",
        .value = DateFormatOption::SECOND,
        .values = {"2-digit", "numeric"},
    },
    {
        .name = "timeZoneName",
        .value = DateFormatOption::TIME_ZONE_NAME,
        .values = {"short", "long", "shortOffset", "longOffset", "shortGeneric", "longGeneric"},
    },
    {
        .name = "formatMatcher",
        .value = DateFormatOption::FORMAT_MATCHER,
        .values = {"basic", "best fit"},
    },
    {
        .name = "dateStyle",
        .value = DateFormatOption::DATE_STYLE,
        .values = {"full", "long", "medium", "short"},
    },
    {
        .name = "timeStyle",
        .value = DateFormatOption::TIME_STYLE,
        .values = {"full", "long", "medium", "short"},
    },
    {
        .name = "hour12",
        .value = DateFormatOption::HOUR12,
        .values = {},
    },
    {
        .name = "fractionalSecondDigits",
        .value = DateFormatOption::FRACTIONAL_SECOND_DIGITS,
        .values = {},
    },
};
}

std::optional<DateFormatOption> DateFormatOptionTable::find(llvm::StringRef name) {
  for (const DateFormatOptionInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

llvm::ArrayRef<DateFormatOptionInfo> DateFormatOptionTable::getOptions() {
  return kRows;
}

llvm::ArrayRef<llvm::StringLiteral> DateFormatOptionTable::getValues(llvm::StringRef name) {
  for (const DateFormatOptionInfo &info : kRows) {
    if (info.name == name) {
      return info.values;
    }
  }

  return {};
}

}
