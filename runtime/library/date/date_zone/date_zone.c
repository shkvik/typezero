#include "runtime/library/intl/icu/icu.h"
#include "runtime/library/date/date_zone/date_zone.h"
#include "runtime/core/parallel/parallel.h"
#include "runtime/core/string/string.h"
#include "runtime/library/date/date/date.h"
#include <stddef.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

TzLock tz_zone_lock;
void *tz_zone_calendar;
int32_t tz_zone_state;
double tz_zone_cache_from, tz_zone_cache_to, tz_zone_cache_offset, tz_zone_cache_dst;
TzUChar tz_zone_names[2][128];
int32_t tz_zone_name_lengths[2];

#ifdef _WIN32
static int32_t windows_zone(const TzIcu *icu, TzUChar *out, int32_t capacity) {
  DYNAMIC_TIME_ZONE_INFORMATION info;
  memset(&info, 0, sizeof info);
  if (GetDynamicTimeZoneInformation(&info) == TIME_ZONE_ID_INVALID) {
    return 0;
  }
  SYSTEMTIME zero;
  memset(&zero, 0, sizeof zero);
  int standard_zero = memcmp(&info.StandardDate, &zero, sizeof zero) == 0;
  if (info.DynamicDaylightTimeDisabled &&
      memcmp(&info.StandardDate, &info.DaylightDate, sizeof zero) == 0 &&
      ((info.TimeZoneKeyName[0] && standard_zero) ||
       (!info.TimeZoneKeyName[0] && !standard_zero))) {
    long bias = info.Bias;
    char name[16] = "UTC";
    if (bias != 0) {
      snprintf(name, sizeof name, "Etc/GMT%+ld", bias / 60);
    }
    if (bias % 60 == 0) {
      int32_t n = 0;
      for (; name[n] && n < capacity; ++n) {
        out[n] = (TzUChar)name[n];
      }
      return n;
    }
  }
  if (!info.TimeZoneKeyName[0]) {
    return 0;
  }
  wchar_t wide[3] = {0, 0, 0};
  char region[3] = {0, 0, 0};
  int has_region = GetGeoInfoW(GetUserGeoID(GEOCLASS_NATION), GEO_ISO2, wide, 3, 0) != 0;
  for (int i = 0; i < 2; ++i) {
    region[i] = (char)wide[i];
  }
  TzUErrorCode status = 0;
  int32_t n = 0;
  if (has_region) {
    n = icu->ucal_getTimeZoneIDForWindowsID((const TzUChar *)info.TimeZoneKeyName, -1, region, out,
                                            capacity, &status);
  }
  if (n <= 0 || TZ_U_FAILURE(status)) {
    status = 0;
    n = icu->ucal_getTimeZoneIDForWindowsID((const TzUChar *)info.TimeZoneKeyName, -1, NULL, out,
                                            capacity, &status);
  }

  return TZ_U_FAILURE(status) || n >= capacity ? 0 : n;
}
#endif

static void *zone_calendar(void) {
  if (tz_zone_state) {
    return tz_zone_calendar;
  }
  tz_zone_state = 1;
  tz_zone_cache_from = 1;
  tz_zone_cache_to = 0;
  tz_zone_name_lengths[0] = tz_zone_name_lengths[1] = -1;
  const TzIcu *icu = tz_icu();
  if (!icu) {
    return NULL;
  }
  TzUChar zone[128];
  int32_t length = 0;
#ifdef _WIN32
  DWORD n = GetEnvironmentVariableW(L"TZ", (wchar_t *)zone, (DWORD)(sizeof zone / sizeof zone[0]));
  if (n > 0 && n < sizeof zone / sizeof zone[0]) {
    length = (int32_t)n;
  } else {
    length = windows_zone(icu, zone, (int32_t)(sizeof zone / sizeof zone[0]));
  }
#endif
  TzUErrorCode status = 0;
  void *cal = icu->ucal_open(length ? zone : NULL, length ? length : -1, tz_icu_default_locale(),
                             TZ_UCAL_GREGORIAN, &status);
  if (TZ_U_FAILURE(status)) {
    return NULL;
  }
  tz_zone_calendar = cal;

  return cal;
}

static void offsets_at(double t, double *offset, double *dst) {
  if (t >= tz_zone_cache_from && t < tz_zone_cache_to) {
    *offset = tz_zone_cache_offset;
    *dst = tz_zone_cache_dst;
    return;
  }
  void *cal = zone_calendar();
  *offset = *dst = 0;
  if (!cal) {
    return;
  }
  const TzIcu *icu = tz_icu();
  TzUErrorCode status = 0;
  icu->ucal_setMillis(cal, t, &status);
  int32_t raw = icu->ucal_get(cal, TZ_UCAL_ZONE_OFFSET, &status);
  int32_t daylight = icu->ucal_get(cal, TZ_UCAL_DST_OFFSET, &status);
  if (TZ_U_FAILURE(status)) {
    return;
  }
  *offset = (double)raw + (double)daylight;
  *dst = daylight;
  double from = t, to = t;
  status = 0;
  if (!icu->ucal_getTimeZoneTransitionDate(cal, TZ_UCAL_TZ_TRANSITION_PREVIOUS_INCLUSIVE, &from,
                                           &status) ||
      TZ_U_FAILURE(status)) {
    from = -INFINITY;
  }
  status = 0;
  if (!icu->ucal_getTimeZoneTransitionDate(cal, TZ_UCAL_TZ_TRANSITION_NEXT, &to, &status) ||
      TZ_U_FAILURE(status)) {
    to = INFINITY;
  }
  if (from > t || to <= t) {
    from = to = t;
  }
  tz_zone_cache_from = from;
  tz_zone_cache_to = to;
  tz_zone_cache_offset = *offset;
  tz_zone_cache_dst = *dst;
}

double tz_date_offset(double t) {
  double offset, dst;
  tz_lock(&tz_zone_lock);
  offsets_at(t, &offset, &dst);
  tz_unlock(&tz_zone_lock);

  return offset;
}

static double local_to_utc(double local);

double tz_date_local_to_utc(double local) {
  tz_lock(&tz_zone_lock);
  double utc = local_to_utc(local);
  tz_unlock(&tz_zone_lock);

  return utc;
}

static double local_to_utc(double local) {
  void *cal = zone_calendar();
  if (!cal) {
    return local;
  }
  const TzIcu *icu = tz_icu();
  if (icu->ucal_getTimeZoneOffsetFromLocal) {
    TzUErrorCode status = 0;
    int32_t raw = 0, dst = 0;
    icu->ucal_setMillis(cal, local, &status);
    icu->ucal_getTimeZoneOffsetFromLocal(cal, TZ_UCAL_TZ_LOCAL_FORMER, TZ_UCAL_TZ_LOCAL_FORMER,
                                         &raw, &dst, &status);
    if (!TZ_U_FAILURE(status)) {
      return local - ((double)raw + (double)dst);
    }
  }
  double before = tz_date_offset(local - 86400000.0);
  double after = tz_date_offset(local + 86400000.0);
  double former = local - before;
  if (before == after || tz_date_offset(former) == before) {
    return former;
  }
  double latter = local - after;

  return tz_date_offset(latter) == after ? latter : former;
}

static void zone_name(TzBuf *out, double t);

void tz_date_zone_name(TzBuf *out, double t) {
  tz_lock(&tz_zone_lock);
  zone_name(out, t);
  tz_unlock(&tz_zone_lock);
}

static void zone_name(TzBuf *out, double t) {
  if (t < 0 || t > 2147483647000.0) {
    t = tz_date_equivalent_time(t);
  }
  double offset, dst;
  offsets_at(t, &offset, &dst);
  void *cal = zone_calendar();
  if (!cal) {
    tz_buf_ascii(out, "Coordinated Universal Time");
    return;
  }
  int daylight = dst != 0;
  if (tz_zone_name_lengths[daylight] < 0) {
    TzUErrorCode status = 0;
    int32_t n = tz_icu()->ucal_getTimeZoneDisplayName(
        cal, daylight ? TZ_UCAL_DST : TZ_UCAL_STANDARD, tz_icu_default_locale(),
        tz_zone_names[daylight], (int32_t)(sizeof tz_zone_names[0] / sizeof(TzUChar)), &status);
    tz_zone_name_lengths[daylight] = TZ_U_FAILURE(status) ? 0 : n;
  }
  tz_buf_units(out, tz_zone_names[daylight], (size_t)tz_zone_name_lengths[daylight]);
}

int32_t tz_date_zone_id(TzUChar *out, int32_t capacity) {
  tz_lock(&tz_zone_lock);
  void *cal = zone_calendar();
  if (!cal) {
    tz_unlock(&tz_zone_lock);
    static const char utc[] = "UTC";
    for (int i = 0; i < 3; ++i) {
      out[i] = (TzUChar)utc[i];
    }
    return 3;
  }
  TzUErrorCode status = 0;
  int32_t n = tz_icu()->ucal_getTimeZoneID(cal, out, capacity, &status);
  tz_unlock(&tz_zone_lock);

  return TZ_U_FAILURE(status) ? 0 : n;
}
