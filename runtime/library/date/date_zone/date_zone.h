#ifndef TZ_DATE_ZONE_H
#define TZ_DATE_ZONE_H

#include "runtime/core/string/string.h"
#include <stddef.h>
#include <stdint.h>

int64_t tz_date_offset(int64_t t);
int64_t tz_date_local_offset(int64_t local);
void tz_date_zone_name(TzBuf *out, int64_t t);

#endif
