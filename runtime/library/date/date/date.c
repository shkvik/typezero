#include "runtime/library/date/date/date.h"
#include "runtime/core/output/output.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/string/string.h"
#include "runtime/library/date/date_zone/date_zone.h"
#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>

#define MS_PER_DAY 86400000

static const char *const week_days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

typedef __int128 Wide;

typedef struct {
  int64_t year;
  int month, day, week_day, hour, minute, second, ms;
  int64_t days;
  int64_t time_in_day;
} Fields;

static int64_t floor_div(int64_t a, int64_t b) {
  int64_t q = a / b;

  return a % b < 0 ? q - 1 : q;
}

static int64_t clamp(Wide v) {
  if (v > (Wide)INT64_MAX) {
    return INT64_MAX;
  }

  return v < (Wide)INT64_MIN ? INT64_MIN : (int64_t)v;
}

static void break_down(int64_t ms, Fields *f) {
  int64_t days = floor_div(ms, MS_PER_DAY);
  f->days = days;
  f->time_in_day = ms - days * MS_PER_DAY;
  int64_t z = days + 719468;
  int64_t era = floor_div(z, 146097);
  int64_t doe = z - era * 146097;
  int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int64_t mp = (5 * doy + 2) / 153;
  int month = (int)(mp < 10 ? mp + 2 : mp - 10);
  f->year = yoe + era * 400 + (month <= 1);
  f->month = month;
  f->day = (int)(doy - (153 * mp + 2) / 5 + 1);
  int64_t wd = (days + 4) % 7;
  f->week_day = (int)(wd < 0 ? wd + 7 : wd);
  f->hour = (int)(f->time_in_day / 3600000);
  f->minute = (int)(f->time_in_day / 60000 % 60);
  f->second = (int)(f->time_in_day / 1000 % 60);
  f->ms = (int)(f->time_in_day % 1000);
}

static Wide month_start(int64_t year, int64_t month) {
  int64_t y = month < 2 ? year - 1 : year;
  int64_t era = floor_div(y, 400);
  int64_t yoe = y - era * 400;
  int64_t doy = (153 * ((month + 10) % 12) + 2) / 5;

  return (Wide)era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}

static int64_t compose(const int64_t parts[7]) {
  int64_t carry = floor_div(parts[1], 12);
  int64_t month = parts[1] - carry * 12;
  Wide year = (Wide)parts[0] + carry;
  const Wide distant = (Wide)100000000000000000;
  if (year > distant || year < -distant) {
    return year > 0 ? INT64_MAX : INT64_MIN;
  }
  Wide days = month_start((int64_t)year, month) + parts[2] - 1;

  return clamp(days * MS_PER_DAY + (Wide)parts[3] * 3600000 + (Wide)parts[4] * 60000 +
               (Wide)parts[5] * 1000 + parts[6]);
}

int64_t tz_date_equivalent_time(int64_t t) {
  Fields f;
  break_down(t, &f);
  int64_t first = (int64_t)month_start(f.year, 0);
  int64_t wd = (first + 4) % 7;
  int week_day = (int)(wd < 0 ? wd + 7 : wd);
  int leap = f.year % 4 == 0 && (f.year % 100 != 0 || f.year % 400 == 0);
  int recent = (leap ? 1956 : 1967) + (week_day * 12) % 28;
  int year = 2008 + (recent + 3 * 28 - 2008) % 28;
  int64_t days = (int64_t)month_start(year, f.month) + f.day - 1;

  return days * MS_PER_DAY + f.time_in_day;
}

static int64_t shift(int64_t t, int64_t by) {
  int64_t out;
  if (__builtin_add_overflow(t, by, &out)) {
    return by > 0 ? INT64_MAX : INT64_MIN;
  }

  return out;
}

static int64_t to_local(int64_t t) { return shift(t, tz_date_offset(t)); }

static int64_t from_local(int64_t local) { return shift(local, -tz_date_local_offset(local)); }

int64_t tz_date_from_fields(int64_t year, int64_t month, int64_t date, int64_t hours,
                            int64_t minutes, int64_t seconds, int64_t ms, int32_t local) {
  int64_t parts[7] = {
      year >= 0 && year <= 99 ? 1900 + year : year, month, date, hours, minutes, seconds, ms};
  int64_t value = compose(parts);

  return local ? from_local(value) : value;
}

static int text_time(void *obj, uint64_t meta, double *out) {
  TzStr s = {obj, meta};
  uint32_t length;
  int two;
  const void *units = tz_str_units(&s, &length, &two);
  uint16_t small[64];
  uint16_t *wide = small;
  if (!two) {
    if (length > 64) {
      wide = (uint16_t *)tz_alloc(length * sizeof(uint16_t));
    }
    for (uint32_t i = 0; i < length; ++i) {
      wide[i] = ((const uint8_t *)units)[i];
    }
  }
  TzDateFields f;
  int ok = tz_date_parse_fields(two ? (const uint16_t *)units : wide, length, &f);
  if (wide != small) {
    tz_free(wide);
  }
  if (!ok) {
    return 0;
  }
  double day = tz_date_make_day(f.year, f.month, f.day);
  double time = tz_date_make_time(f.hour, f.minute, f.second, f.ms);
  double value = tz_date_make_date(day, time);
  if (!(value >= -TZ_DATE_MAX_LOCAL_TIME && value <= TZ_DATE_MAX_LOCAL_TIME)) {
    return 0;
  }
  *out = isnan(f.offset) ? (double)from_local((int64_t)value) : value - f.offset * 1000.0;

  return 1;
}

double tz_date_parse(void *obj, uint64_t meta) {
  double value;
  if (!text_time(obj, meta, &value)) {
    return NAN;
  }

  return tz_date_time_clip(value);
}

int64_t tz_date_from_text(void *obj, uint64_t meta) {
  double value;

  return text_time(obj, meta, &value) ? (int64_t)value : 0;
}

static int64_t field_of(const Fields *f, int32_t field) {
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

int64_t tz_date_utc_field(int64_t t, int32_t field) {
  Fields f;
  break_down(t, &f);

  return field_of(&f, field);
}

int64_t tz_date_local_field(int64_t t, int32_t field) {
  if (field == TZ_DATE_OFFSET) {
    return -tz_date_offset(t) / 60000;
  }
  Fields f;
  break_down(to_local(t), &f);

  return field_of(&f, field);
}

int64_t tz_date_set(int64_t t, int32_t field, int32_t local, int32_t count, int64_t a, int64_t b,
                    int64_t c, int64_t d) {
  int64_t args[4] = {a, b, c, d};
  Fields f;
  break_down(local ? to_local(t) : t, &f);
  int64_t parts[7] = {f.year, f.month, f.day, f.hour, f.minute, f.second, f.ms};
  int first = field <= TZ_DATE_DATE ? field : field - 1;
  for (int i = 0; i < count && first + i < 7; ++i) {
    parts[first + i] = args[i];
  }
  int64_t value = compose(parts);

  return local ? from_local(value) : value;
}

static TzStr ascii(const char *text, int length) {
  return tz_str_from_ascii(text, length > 0 ? (size_t)length : 0);
}

static int iso_text(char *out, size_t size, int64_t t) {
  Fields f;
  break_down(t, &f);
  if (f.year >= 0 && f.year <= 9999) {
    return snprintf(out, size, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", (int)f.year, f.month + 1,
                    f.day, f.hour, f.minute, f.second, f.ms);
  }

  return snprintf(out, size, "%c%06lld-%02d-%02dT%02d:%02d:%02d.%03dZ", f.year < 0 ? '-' : '+',
                  (long long)(f.year < 0 ? -f.year : f.year), f.month + 1, f.day, f.hour, f.minute,
                  f.second, f.ms);
}

void tz_date_iso(TzStr *out, int64_t t) {
  char text[40];
  *out = ascii(text, iso_text(text, sizeof text, t));
}

void tz_buf_date(TzBuf *out, int64_t t) {
  char text[40];
  int n = iso_text(text, sizeof text, t);
  tz_buf_ascii_n(out, text, n > 0 ? (size_t)n : 0);
}

void tz_date_utc_string(TzStr *out, int64_t t) {
  Fields f;
  break_down(t, &f);
  char text[64];
  int n = snprintf(text, sizeof text,
                   f.year < 0 ? "%s, %02d %s %05lld %02d:%02d:%02d GMT"
                              : "%s, %02d %s %04lld %02d:%02d:%02d GMT",
                   week_days[f.week_day], f.day, months[f.month], (long long)f.year, f.hour,
                   f.minute, f.second);
  *out = ascii(text, n);
}

void tz_date_local_string(TzStr *out, int64_t t, int32_t mode) {
  int64_t zone = tz_date_offset(t);
  Fields f;
  break_down(shift(t, zone), &f);
  int offset = (int)(zone / 60000);
  int offset_hours = (offset < 0 ? -offset : offset) / 60;
  int offset_minutes = (offset < 0 ? -offset : offset) % 60;
  char text[80];
  int n = 0;
  if (mode != TZ_DATE_TEXT_TIME) {
    n = snprintf(text, sizeof text, f.year < 0 ? "%s %s %02d %05lld" : "%s %s %02d %04lld",
                 week_days[f.week_day], months[f.month], f.day, (long long)f.year);
  }
  if (mode == TZ_DATE_TEXT_DATE) {
    *out = ascii(text, n);
    return;
  }
  if (mode != TZ_DATE_TEXT_TIME) {
    text[n++] = ' ';
  }
  n += snprintf(text + n, sizeof text - (size_t)n, "%02d:%02d:%02d GMT%c%02d%02d (", f.hour,
                f.minute, f.second, offset < 0 ? '-' : '+', offset_hours, offset_minutes);
  TzBuf b;
  tz_buf_init(&b);
  tz_buf_ascii_n(&b, text, (size_t)n);
  tz_date_zone_name(&b, t);
  tz_buf_push(&b, ')');
  *out = tz_buf_take(&b);
}
