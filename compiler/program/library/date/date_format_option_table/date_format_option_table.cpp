#include "compiler/program/library/date/date_format_option_table/date_format_option_table.h"

namespace tz {

namespace {

const DateFormatOptionInfo kRows[] = {
    {
        .name = "localeMatcher",
        .value = DateFormatOption::LOCALE_MATCHER,
    },
    {
        .name = "calendar",
        .value = DateFormatOption::CALENDAR,
    },
    {
        .name = "numberingSystem",
        .value = DateFormatOption::NUMBERING_SYSTEM,
    },
    {
        .name = "hourCycle",
        .value = DateFormatOption::HOUR_CYCLE,
    },
    {
        .name = "timeZone",
        .value = DateFormatOption::TIME_ZONE,
    },
    {
        .name = "weekday",
        .value = DateFormatOption::WEEKDAY,
    },
    {
        .name = "era",
        .value = DateFormatOption::ERA,
    },
    {
        .name = "year",
        .value = DateFormatOption::YEAR,
    },
    {
        .name = "month",
        .value = DateFormatOption::MONTH,
    },
    {
        .name = "day",
        .value = DateFormatOption::DAY,
    },
    {
        .name = "dayPeriod",
        .value = DateFormatOption::DAY_PERIOD,
    },
    {
        .name = "hour",
        .value = DateFormatOption::HOUR,
    },
    {
        .name = "minute",
        .value = DateFormatOption::MINUTE,
    },
    {
        .name = "second",
        .value = DateFormatOption::SECOND,
    },
    {
        .name = "timeZoneName",
        .value = DateFormatOption::TIME_ZONE_NAME,
    },
    {
        .name = "formatMatcher",
        .value = DateFormatOption::FORMAT_MATCHER,
    },
    {
        .name = "dateStyle",
        .value = DateFormatOption::DATE_STYLE,
    },
    {
        .name = "timeStyle",
        .value = DateFormatOption::TIME_STYLE,
    },
    {
        .name = "hour12",
        .value = DateFormatOption::HOUR12,
    },
    {
        .name = "fractionalSecondDigits",
        .value = DateFormatOption::FRACTIONAL_SECOND_DIGITS,
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

}
