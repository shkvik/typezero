#ifndef TZ_DECIMAL_H
#define TZ_DECIMAL_H

#include "runtime/core/string/string.h"
#include <stddef.h>
#include <stdint.h>

typedef struct {
  int64_t coef;
  uint64_t meta;
} TzDec;

#define TZ_DEC_HEAP (UINT64_C(1) << 63)
#define TZ_DEC_LONG (UINT64_C(1) << 62)

typedef struct {
  int64_t rc;
  int64_t exp;
  int32_t neg;
  uint32_t n;
  uint32_t d[];
} TzDecObj;

int64_t tz_dec_live_objects(void);
void tz_dec_retain_value(TzDec v);
void tz_dec_release_value(TzDec v);
int32_t tz_dec_equal(int64_t a, uint64_t am, int64_t b, uint64_t bm);
void tz_buf_decimal(TzBuf *out, TzDec v);
void tz_dec_from_string(TzDec *out, void *obj, uint64_t meta);

#endif
