// The local zone.

#ifndef TZ_DATE_ZONE_H
#define TZ_DATE_ZONE_H

#include "runtime/core/string/string.h"
#include <stddef.h>
#include <stdint.h>

// The local zone (date_zone.c): the offset of local time from UTC at the
// moment t, in milliseconds; the moment of a local time; the zone's name at
// t, as toString() writes it.
double tz_date_offset(double t);
double tz_date_local_to_utc(double local);
void tz_date_zone_name(TzBuf *out, double t);

#endif
