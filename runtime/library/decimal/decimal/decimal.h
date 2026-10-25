// Exact decimal numbers (docs/decimal.md).

#ifndef TZ_DECIMAL_H
#define TZ_DECIMAL_H

#include "runtime/core/string/string.h"
#include <stddef.h>
#include <stdint.h>

// A decimal value is two words, as a string is. A number of up to 18 digits
// or so is `coef * 10^-scale` in the value itself, with the scale in the low
// 32 bits of `meta` and no flags; longer ones keep their digits in a TzDecObj
// that `coef` points to (see decimal.c). Must match Lowering
// (compiler/library/decimal/codegen/codegen_decimal.cpp).
typedef struct {
  int64_t coef;
  uint64_t meta;
} TzDec;

#define TZ_DEC_HEAP (UINT64_C(1) << 63) // coef points to a counted TzDecObj
#define TZ_DEC_LONG (UINT64_C(1) << 62) // coef points to a TzDecObj

// (-1)^neg * d * 10^exp, where d is the integer whose base 10^9 digits are
// d[0] (the lowest) to d[n - 1].
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
// The text String(v) gives, appended to `out`.
void tz_buf_decimal(TzBuf *out, TzDec v);
void tz_dec_from_string(TzDec *out, void *obj, uint64_t meta);

#endif
