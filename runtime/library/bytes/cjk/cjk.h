// The multi-byte encodings of TextDecoder (the WHATWG Encoding Standard).

#ifndef TZ_CJK_H
#define TZ_CJK_H

#include "runtime/core/string/string.h"
#include <stddef.h>
#include <stdint.h>

// A multi-byte encoding of TextDecoder (cjk.c): which decoder of the WHATWG
// Encoding Standard, and the indexes it reads, each the code points of the
// pointers from `first` on (0 for none). A Big5 code point is U+2xxxx where
// its bit in `astral` is set. EUC-JP has jis0212 as its second index; gb18030
// (and gbk) has its ranges, pairs of a pointer and a code point. `decode` is
// tz_cjk_decode, which a program gets only with an encoding it names.
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
// Appends the text of `length` bytes to `out`, going on from `state` (four
// words, zero at the start of a stream) and leaving it there; unless
// `stream`, the bytes end the stream. Returns 0 at the first error when
// `fatal`, 1 otherwise.
int32_t tz_cjk_decode(const TzCjkEncoding *encoding, uint32_t *state, TzBuf *out,
                      const uint8_t *data, int64_t length, int32_t stream, int32_t fatal);

#endif
