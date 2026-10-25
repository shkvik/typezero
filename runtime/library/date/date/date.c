// Dates (docs/date.md). A Date is an object of the language whose one field
// is its time value: milliseconds since 1970 in UTC, NaN for an invalid date
// (see TzDate in tzrt.h). The compiler makes and changes the objects; here is
// the arithmetic of time values, as V8 computes it (src/date, and
// builtins-date.cc): taking them apart into fields, the setters, the strings
// of toISOString, toUTCString and toString, and reading strings.
//
// What is local needs the rules of the time zone, which come from
// date_zone.c: only the functions of local time call there, so a program
// that uses only UTC (and Date.now()) never loads them.

#include "runtime/library/date/date/date.h"
#include "runtime/core/tzrt/tzrt.h"

#include <math.h>
#include <stdio.h>

#define MS_PER_DAY 86400000

static const char *const week_days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// A time value taken apart. `t` is a whole number of milliseconds within the
// limits, local or UTC.
typedef struct {
  int year, month, day, week_day, hour, minute, second, ms;
  int64_t days;        // since 1970
  int64_t time_in_day; // milliseconds since midnight
} Fields;

static void break_down(double t, Fields *f) {
  int64_t ms = (int64_t)t;
  int64_t days = ms >= 0 ? ms / MS_PER_DAY : (ms - (MS_PER_DAY - 1)) / MS_PER_DAY;
  f->days = days;
  f->time_in_day = ms - days * MS_PER_DAY;
  // The proleptic Gregorian calendar from days (H. Hinnant, "chrono-Compatible
  // Low-Level Date Algorithms").
  int64_t z = days + 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  int64_t doe = z - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int64_t mp = (5 * doy + 2) / 153;
  int month = (int)(mp < 10 ? mp + 2 : mp - 10);
  f->year = (int)(yoe + era * 400 + (month <= 1));
  f->month = month;
  f->day = (int)(doy - (153 * mp + 2) / 5 + 1);
  int64_t wd = (days + 4) % 7; // 1970-01-01 was a Thursday
  f->week_day = (int)(wd < 0 ? wd + 7 : wd);
  f->hour = (int)(f->time_in_day / 3600000);
  f->minute = (int)(f->time_in_day / 60000 % 60);
  f->second = (int)(f->time_in_day / 1000 % 60);
  f->ms = (int)(f->time_in_day % 1000);
}

// The same day and time in a year from 2008 to 2035 that is a leap year if
// t's is and starts on the same day of the week (V8's EquivalentTime). V8
// takes the name of the zone of a time before 1970 or after 2038 as for that
// year: in standard or summer time as the zone is then today.
double tz_date_equivalent_time(double t) {
  Fields f;
  break_down(t, &f);
  double first = tz_date_make_day(f.year, 0, 1);
  int64_t wd = ((int64_t)first + 4) % 7;
  int week_day = (int)(wd < 0 ? wd + 7 : wd);
  int leap = f.year % 4 == 0 && (f.year % 100 != 0 || f.year % 400 == 0);
  int recent = (leap ? 1956 : 1967) + (week_day * 12) % 28;
  int year = 2008 + (recent + 3 * 28 - 2008) % 28;
  double days = tz_date_make_day(year, f.month, 1) + f.day - 1;
  return days * MS_PER_DAY + (double)f.time_in_day;
}

// The local time of a valid time value.
static double to_local(double t) { return t + tz_date_offset(t); }

// A local time made from fields as UTC: the time value, NaN if out of range
// (V8's SetLocalDateValue).
static double from_local(double local) {
  if (!(local >= -TZ_DATE_MAX_LOCAL_TIME && local <= TZ_DATE_MAX_LOCAL_TIME))
    return NAN;
  return tz_date_time_clip(tz_date_local_to_utc(local));
}

//===----------------------------------------------------------------------===//
// Making dates
//===----------------------------------------------------------------------===//

// new Date(y, m, d, h, min, s, ms) with `local`, and Date.UTC: the
// arguments left out are given as their defaults. A year from 0 to 99 is
// 1900 and more.
double tz_date_from_fields(double year, double month, double date, double hours, double minutes,
                           double seconds, double ms, int32_t local) {
  if (!isnan(year)) {
    double y = trunc(year);
    if (y >= 0 && y <= 99)
      year = 1900 + y;
  }
  double day = tz_date_make_day(year, month, date);
  double time = tz_date_make_time(hours, minutes, seconds, ms);
  double value = tz_date_make_date(day, time);
  return local ? from_local(value) : tz_date_time_clip(value);
}

double tz_date_clip(double t) { return tz_date_time_clip(t); }

// Date.parse(s) and new Date(s).
double tz_date_parse(void *obj, uint64_t meta) {
  TzStr s = {obj, meta};
  uint32_t length;
  int two;
  const void *units = tz_str_units(&s, &length, &two);
  uint16_t small[64];
  uint16_t *wide = small;
  if (!two) {
    if (length > 64)
      wide = (uint16_t *)tz_alloc(length * sizeof(uint16_t));
    for (uint32_t i = 0; i < length; ++i)
      wide[i] = ((const uint8_t *)units)[i];
  }
  TzDateFields f;
  int ok = tz_date_parse_fields(two ? (const uint16_t *)units : wide, length, &f);
  if (wide != small)
    tz_free(wide);
  if (!ok)
    return NAN;
  double day = tz_date_make_day(f.year, f.month, f.day);
  double time = tz_date_make_time(f.hour, f.minute, f.second, f.ms);
  double value = tz_date_make_date(day, time);
  if (isnan(f.offset))
    return from_local(value);
  value -= f.offset * 1000.0;
  if (!(value >= -TZ_DATE_MAX_TIME && value <= TZ_DATE_MAX_TIME))
    return NAN;
  return tz_date_time_clip(value);
}

//===----------------------------------------------------------------------===//
// Fields
//===----------------------------------------------------------------------===//

static double field_of(const Fields *f, int32_t field) {
  switch (field) {
  case TZ_DATE_YEAR:
    return f->year;
  case TZ_DATE_MONTH:
    return f->month;
  case TZ_DATE_DATE:
    return f->day;
  case TZ_DATE_DAY:
    return f->week_day;
  case TZ_DATE_HOURS:
    return f->hour;
  case TZ_DATE_MINUTES:
    return f->minute;
  case TZ_DATE_SECONDS:
    return f->second;
  default:
    return f->ms;
  }
}

// getUTCFullYear() and the others.
double tz_date_utc_field(double t, int32_t field) {
  if (isnan(t))
    return NAN;
  Fields f;
  break_down(t, &f);
  return field_of(&f, field);
}

// getFullYear() and the others, and getTimezoneOffset(): whole minutes, as
// V8 gives them, also of the offsets with seconds that times before
// standard time had.
double tz_date_local_field(double t, int32_t field) {
  if (isnan(t))
    return NAN;
  double local = to_local(t);
  if (field == TZ_DATE_OFFSET)
    return (double)(((int64_t)t - (int64_t)local) / 60000);
  Fields f;
  break_down(local, &f);
  return field_of(&f, field);
}

// The setters: setHours(h, m?, s?, ms?) and the others, with `count` of the
// arguments `a`..`d` given. As in V8 (builtins-date.cc), each changes the
// fields it names in the local time (or UTC), and the result is clipped.
double tz_date_set(double t, int32_t field, int32_t local, int32_t count, double a, double b,
                   double c, double d) {
  double args[4] = {a, b, c, d};
  if (isnan(t)) {
    // setFullYear starts from +0 then, the others give NaN.
    if (field != TZ_DATE_YEAR)
      return NAN;
    t = 0;
  } else if (local) {
    t = to_local(t);
  }
  Fields f;
  break_down(t, &f);
  double h = f.hour, m = f.minute, s = f.second, ms = f.ms;
  double year = f.year, month = f.month, date = f.day;
  double day = (double)f.days, time = (double)f.time_in_day;
  switch (field) {
  case TZ_DATE_MS:
    ms = args[0];
    time = tz_date_make_time(h, m, s, ms);
    break;
  case TZ_DATE_SECONDS:
    s = args[0];
    if (count > 1)
      ms = args[1];
    time = tz_date_make_time(h, m, s, ms);
    break;
  case TZ_DATE_MINUTES:
    m = args[0];
    if (count > 1)
      s = args[1];
    if (count > 2)
      ms = args[2];
    time = tz_date_make_time(h, m, s, ms);
    break;
  case TZ_DATE_HOURS:
    h = args[0];
    if (count > 1)
      m = args[1];
    if (count > 2)
      s = args[2];
    if (count > 3)
      ms = args[3];
    time = tz_date_make_time(h, m, s, ms);
    break;
  case TZ_DATE_DATE:
    day = tz_date_make_day(year, month, args[0]);
    break;
  case TZ_DATE_MONTH:
    month = args[0];
    if (count > 1)
      date = args[1];
    day = tz_date_make_day(year, month, date);
    break;
  default: // TZ_DATE_YEAR
    year = args[0];
    if (count > 1)
      month = args[1];
    if (count > 2)
      date = args[2];
    day = tz_date_make_day(year, month, date);
    break;
  }
  double value = tz_date_make_date(day, time);
  return local ? from_local(value) : tz_date_time_clip(value);
}

//===----------------------------------------------------------------------===//
// Strings
//===----------------------------------------------------------------------===//

static TzStr ascii(const char *text, int length) {
  return tz_str_from_ascii(text, length > 0 ? (size_t)length : 0);
}

// `2026-10-22T18:00:00.000Z`; years past 9999 and before 0 with six digits
// and a sign.
static int iso_text(char *out, size_t size, double t) {
  Fields f;
  break_down(t, &f);
  if (f.year >= 0 && f.year <= 9999)
    return snprintf(out, size, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", f.year, f.month + 1, f.day,
                    f.hour, f.minute, f.second, f.ms);
  return snprintf(out, size, "%c%06d-%02d-%02dT%02d:%02d:%02d.%03dZ", f.year < 0 ? '-' : '+',
                  f.year < 0 ? -f.year : f.year, f.month + 1, f.day, f.hour, f.minute, f.second,
                  f.ms);
}

// toISOString(): of an invalid date, a RangeError (see tz_throw) and the
// empty string.
void tz_date_iso(TzStr *out, double t) {
  if (isnan(t)) {
    tz_throw(TZ_RANGE_ERROR, "Invalid time value");
    *out = ascii("", 0);
    return;
  }
  char text[40];
  *out = ascii(text, iso_text(text, sizeof text, t));
}

// toJSON(): the ISO text, or nothing (null in JavaScript) for an invalid
// date.
int32_t tz_date_json(TzStr *out, double t) {
  if (isnan(t)) {
    out->obj = NULL;
    out->meta = 0;
    return 0;
  }
  char text[40];
  *out = ascii(text, iso_text(text, sizeof text, t));
  return 1;
}

// How console.log shows a date: its ISO text, or `Invalid Date`.
void tz_buf_date(TzBuf *out, double t) {
  if (isnan(t)) {
    tz_buf_ascii(out, "Invalid Date");
    return;
  }
  char text[40];
  int n = iso_text(text, sizeof text, t);
  tz_buf_ascii_n(out, text, n > 0 ? (size_t)n : 0);
}

// `Fri, 23 Oct 2026 10:00:00 GMT`.
void tz_date_utc_string(TzStr *out, double t) {
  if (isnan(t)) {
    *out = ascii("Invalid Date", 12);
    return;
  }
  Fields f;
  break_down(t, &f);
  char text[48];
  int n = snprintf(
      text, sizeof text,
      f.year < 0 ? "%s, %02d %s %05d %02d:%02d:%02d GMT" : "%s, %02d %s %04d %02d:%02d:%02d GMT",
      week_days[f.week_day], f.day, months[f.month], f.year, f.hour, f.minute, f.second);
  *out = ascii(text, n);
}

// toString(), toDateString() and toTimeString() (TZ_DATE_TEXT_*): `Fri Oct 23
// 2026 10:00:00 GMT+0600 (Kyrgyzstan Time)`, its date, its time; with the
// name of the zone in the language of the system, as V8 writes it.
void tz_date_local_string(TzStr *out, double t, int32_t mode) {
  if (isnan(t)) {
    *out = ascii("Invalid Date", 12);
    return;
  }
  double local = to_local(t);
  Fields f;
  break_down(local, &f);
  int offset = (int)(((int64_t)local - (int64_t)t) / 60000);
  int offset_hours = (offset < 0 ? -offset : offset) / 60;
  int offset_minutes = (offset < 0 ? -offset : offset) % 60;
  char text[64];
  int n = 0;
  if (mode != TZ_DATE_TEXT_TIME)
    n = snprintf(text, sizeof text, f.year < 0 ? "%s %s %02d %05d" : "%s %s %02d %04d",
                 week_days[f.week_day], months[f.month], f.day, f.year);
  if (mode == TZ_DATE_TEXT_DATE) {
    *out = ascii(text, n);
    return;
  }
  if (mode != TZ_DATE_TEXT_TIME)
    text[n++] = ' ';
  n += snprintf(text + n, sizeof text - (size_t)n, "%02d:%02d:%02d GMT%c%02d%02d (", f.hour,
                f.minute, f.second, offset < 0 ? '-' : '+', offset_hours, offset_minutes);
  TzBuf b;
  tz_buf_init(&b);
  tz_buf_ascii_n(&b, text, (size_t)n);
  tz_date_zone_name(&b, t);
  tz_buf_push(&b, ')');
  *out = tz_buf_take(&b);
}
