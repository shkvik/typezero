// Dates (docs/date.md): what the parser of date strings (date_parse.c) gives,
// and the arithmetic of time values that tz needs too.
//
// One parser serves both ends: tz checks the date strings written in a
// program with it (date_parse.c is built into tz as well), and the runtime
// reads strings with it when the program runs, so a string tz accepts is read
// the same way later.

#ifndef TZ_DATE_H
#define TZ_DATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The fields of a time value, for the getters and the setters of Date.
enum {
  TZ_DATE_YEAR = 0,
  TZ_DATE_MONTH = 1,
  TZ_DATE_DATE = 2,
  TZ_DATE_DAY = 3, // of the week
  TZ_DATE_HOURS = 4,
  TZ_DATE_MINUTES = 5,
  TZ_DATE_SECONDS = 6,
  TZ_DATE_MS = 7,
  TZ_DATE_OFFSET = 8, // getTimezoneOffset()
};

// What the texts of local time are: toString(), toDateString(),
// toTimeString().
enum { TZ_DATE_TEXT_FULL = 0, TZ_DATE_TEXT_DATE = 1, TZ_DATE_TEXT_TIME = 2 };

// The methods that write a date in the format of a language, which decide
// what is shown by default (ECMA-402, CreateDateTimeFormat): toLocaleString,
// toLocaleDateString, toLocaleTimeString and new Intl.DateTimeFormat.
enum {
  TZ_DATE_LOCALE_ALL = 0,
  TZ_DATE_LOCALE_DATE = 1,
  TZ_DATE_LOCALE_TIME = 2,
  TZ_DATE_LOCALE_FORMAT = 3,
};

// The options of Intl.DateTimeFormat that are strings, in the order the
// runtime takes them (TzDateOptions in tzrt.h).
enum {
  TZ_DTO_LOCALE_MATCHER,
  TZ_DTO_CALENDAR,
  TZ_DTO_NUMBERING_SYSTEM,
  TZ_DTO_HOUR_CYCLE,
  TZ_DTO_TIME_ZONE,
  TZ_DTO_WEEKDAY,
  TZ_DTO_ERA,
  TZ_DTO_YEAR,
  TZ_DTO_MONTH,
  TZ_DTO_DAY,
  TZ_DTO_DAY_PERIOD,
  TZ_DTO_HOUR,
  TZ_DTO_MINUTE,
  TZ_DTO_SECOND,
  TZ_DTO_TIME_ZONE_NAME,
  TZ_DTO_FORMAT_MATCHER,
  TZ_DTO_DATE_STYLE,
  TZ_DTO_TIME_STYLE,
  TZ_DTO_COUNT
};

// The fields of a date string as V8's DateParser gives them: the month from
// 0, and the offset from UTC in seconds, NaN for local time.
typedef struct {
  double year, month, day, hour, minute, second, ms;
  double offset;
  // An ISO date without a time ("2026-10-23", "2026-10", "2026"), which the
  // standard reads as UTC although a date with a time is local.
  int date_only;
} TzDateFields;

// Reads a date string; 0 if it is not one (`Invalid Date`).
int tz_date_parse_fields(const uint16_t *units, size_t length, TzDateFields *out);

// The operations of the standard on time values (ECMA-262, 21.4.1), as V8
// computes them: NaN for what is out of range.
double tz_date_make_day(double year, double month, double date);
double tz_date_make_time(double hour, double min, double sec, double ms);
double tz_date_make_date(double day, double time);
double tz_date_time_clip(double time);

// The limits of time values: ±8.64e15 ms, and for a local time before it
// becomes UTC ten days more.
#define TZ_DATE_MAX_TIME 8.64e15
#define TZ_DATE_MAX_LOCAL_TIME (8.64e15 + 864000000.0)

#ifdef __cplusplus
}
#endif

// What follows is for the runtime only.
#ifndef __cplusplus

#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"

// A Date: an object of the language whose one field is its time value, in
// milliseconds since 1970 in UTC, NaN for an invalid date. The compiler makes
// these objects and reads and writes the field. Must match Lowering
// (compiler/library/date/codegen/codegen_date.cpp).
typedef struct TzDate {
  int64_t rc;
  const TzLayout *layout; // with TZ_LAYOUT_DATE
  double time;
} TzDate;

// How console.log shows a date.
void tz_buf_date(TzBuf *out, double t);
// toJSON(), as JSON.stringify writes a date: the ISO text, or 0 for an
// invalid date (null).
int32_t tz_date_json(TzStr *out, double t);
// toString() and the like, in local time: `mode` is TZ_DATE_TEXT_* of date.h,
// 0 for toString(), which join() writes.
void tz_date_local_string(TzStr *out, double t, int32_t mode);

// The same day and time in an equivalent year of 2008 to 2035 (date.c).
double tz_date_equivalent_time(double t);

#endif

#endif
