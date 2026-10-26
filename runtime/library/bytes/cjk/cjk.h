#ifndef TZ_CJK_H
#define TZ_CJK_H

#include "runtime/core/string/string.h"
#include <stddef.h>
#include <stdint.h>

enum {
  TZ_CJK_KIND_GB18030,
  TZ_CJK_KIND_BIG5,
  TZ_CJK_KIND_EUC_JP,
  TZ_CJK_KIND_ISO_2022_JP,
  TZ_CJK_KIND_SHIFT_JIS,
  TZ_CJK_KIND_EUC_KR,
};
typedef struct TzCjkEncoding {
  const char *name;
  int32_t kind;
  uint32_t first, count;
  const uint16_t *table;
  const uint8_t *astral;
  uint32_t first2, count2;
  const uint16_t *table2;
  const uint32_t *ranges;
  uint32_t range_count;
  int32_t (*decode)(const struct TzCjkEncoding *encoding, uint32_t *state, TzBuf *out,
                    const uint8_t *data, int64_t length, int32_t stream, int32_t fatal);
} TzCjkEncoding;
int32_t tz_cjk_decode(const TzCjkEncoding *encoding, uint32_t *state, TzBuf *out,
                      const uint8_t *data, int64_t length, int32_t stream, int32_t fatal);

#endif
