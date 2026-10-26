#ifndef TZ_DATE_ZONE_H
#define TZ_DATE_ZONE_H

#include "runtime/core/string/string.h"
#include <stddef.h>
#include <stdint.h>

double tz_date_offset(double t);
double tz_date_local_to_utc(double local);
void tz_date_zone_name(TzBuf *out, double t);

#endif
