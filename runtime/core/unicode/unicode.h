#ifndef TZ_UNICODE_H
#define TZ_UNICODE_H

#include <stddef.h>
#include <stdint.h>

int tz_case_map(uint32_t cp, int upper, uint32_t out[3]);
int tz_is_cased(uint32_t cp);
int tz_is_case_ignorable(uint32_t cp);
int tz_is_js_space(uint32_t unit);

static inline int tz_js_space(uint32_t unit) {
  switch (unit) {
  case 0x09:
  case 0x0A:
  case 0x0B:
  case 0x0C:
  case 0x0D:
  case 0x20:
  case 0xA0:
  case 0x1680:
  case 0x2028:
  case 0x2029:
  case 0x202F:
  case 0x205F:
  case 0x3000:
  case 0xFEFF:
    return 1;
  default:
    return unit >= 0x2000 && unit <= 0x200A;
  }
}

#endif
