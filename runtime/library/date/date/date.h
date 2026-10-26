#ifndef TZ_DATE_H
#define TZ_DATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  TZ_DATE_YEAR = 0,
  TZ_DATE_MONTH = 1,
  TZ_DATE_DATE = 2,
  TZ_DATE_DAY = 3,
  TZ_DATE_HOURS = 4,
  TZ_DATE_MINUTES = 5,
  TZ_DATE_SECONDS = 6,
  TZ_DATE_MS = 7,
  TZ_DATE_OFFSET = 8,
};

enum { TZ_DATE_TEXT_FULL = 0, TZ_DATE_TEXT_DATE = 1, TZ_DATE_TEXT_TIME = 2 };

enum {
  TZ_DATE_LOCALE_ALL = 0,
  TZ_DATE_LOCALE_DATE = 1,
  TZ_DATE_LOCALE_TIME = 2,
  TZ_DATE_LOCALE_FORMAT = 3,
};

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

typedef struct {
  double year, month, day, hour, minute, second, ms;
  double offset;
  int date_only;
} TzDateFields;

int tz_date_parse_fields(const uint16_t *units, size_t length, TzDateFields *out);

double tz_date_make_day(double year, double month, double date);
double tz_date_make_time(double hour, double min, double sec, double ms);
double tz_date_make_date(double day, double time);
double tz_date_time_clip(double time);

#define TZ_DATE_MAX_TIME 8.64e15
#define TZ_DATE_MAX_LOCAL_TIME (8.64e15 + 864000000.0)

#ifdef __cplusplus
}
#endif

#ifndef __cplusplus

#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"

typedef struct TzDate {
  int64_t rc;
  const TzLayout *layout;
  double time;
} TzDate;

void tz_buf_date(TzBuf *out, double t);
int32_t tz_date_json(TzStr *out, double t);
void tz_date_local_string(TzStr *out, double t, int32_t mode);

double tz_date_equivalent_time(double t);

#endif

#endif
