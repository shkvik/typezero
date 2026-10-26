#ifndef TZ_UNICODE_H
#define TZ_UNICODE_H

#include <stddef.h>
#include <stdint.h>

int tz_case_map(uint32_t cp, int upper, uint32_t out[3]);
int tz_is_cased(uint32_t cp);
int tz_is_case_ignorable(uint32_t cp);
int tz_is_js_space(uint32_t unit);

#endif
