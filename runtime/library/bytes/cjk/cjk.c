#include "runtime/library/bytes/cjk/cjk.h"
#include "runtime/core/string/string.h"
#include <stddef.h>
#include <stdint.h>
#include "runtime/library/bytes/cjk/cjk_tables.h"

enum { GO, ERROR, FINISHED };

#define END (-1)

typedef struct {
  int back[8];
  int count;
  const uint8_t *data;
  int64_t length, at;
} Queue;

static int next_byte(Queue *q, int stream) {
  if (q->count > 0) {
    return q->back[--q->count];
  }
  if (q->at < q->length) {
    return q->data[q->at++];
  }

  return stream ? -2 : END;
}

static void restore(Queue *q, const int *bytes, int n) {
  for (int k = n; k-- > 0;) {
    if (bytes[k] != END) {
      q->back[q->count++] = bytes[k];
    }
  }
}

static void emit(TzBuf *out, uint32_t cp) {
  if (cp >= 0x10000) {
    cp -= 0x10000;
    tz_buf_push(out, (uint16_t)(0xD800 + (cp >> 10)));
    tz_buf_push(out, (uint16_t)(0xDC00 + (cp & 0x3FF)));
  } else {
    tz_buf_push(out, (uint16_t)cp);
  }
}

static int is_ascii(int b) { return b >= 0 && b <= 0x7F; }

static uint32_t index_cp(const uint16_t *table, uint32_t first, uint32_t count, int64_t pointer) {
  if (pointer < first || pointer - first >= count) {
    return 0;
  }

  return table[pointer - first];
}

static uint32_t gb18030_ranges(const TzCjkEncoding *e, int64_t pointer) {
  if ((pointer > 39419 && pointer < 189000) || pointer > 1237575) {
    return 0;
  }
  if (pointer == 7457) {
    return 0xE7C7;
  }
  uint32_t lo = 0, hi = e->range_count;
  while (hi - lo > 1) {
    uint32_t mid = (lo + hi) / 2;
    if (e->ranges[2 * mid] <= pointer) {
      lo = mid;
    } else {
      hi = mid;
    }
  }

  return (uint32_t)(e->ranges[2 * lo + 1] + pointer - e->ranges[2 * lo]);
}

static int gb18030_byte(const TzCjkEncoding *e, uint32_t *s, Queue *q, TzBuf *out, int b) {
  uint32_t *first = &s[0], *second = &s[1], *third = &s[2];
  if (b == END && !*first && !*second && !*third) {
    return FINISHED;
  }
  if (b == END) {
    *first = *second = *third = 0;
    return ERROR;
  }
  if (*third) {
    if (b < 0x30 || b > 0x39) {
      int bytes[3] = {(int)*second, (int)*third, b};
      restore(q, bytes, 3);
      *first = *second = *third = 0;
      return ERROR;
    }
    uint32_t cp = gb18030_ranges(e, ((int64_t)*first - 0x81) * (10 * 126 * 10) +
                                        ((int64_t)*second - 0x30) * (10 * 126) +
                                        ((int64_t)*third - 0x81) * 10 + b - 0x30);
    *first = *second = *third = 0;
    if (!cp) {
      return ERROR;
    }
    emit(out, cp);
    return GO;
  }
  if (*second) {
    if (b >= 0x81 && b <= 0xFE) {
      *third = (uint32_t)b;
      return GO;
    }
    int bytes[2] = {(int)*second, b};
    restore(q, bytes, 2);
    *first = *second = 0;
    return ERROR;
  }
  if (*first) {
    if (b >= 0x30 && b <= 0x39) {
      *second = (uint32_t)b;
      return GO;
    }
    uint32_t lead = *first;
    *first = 0;
    int64_t pointer = -1;
    int offset = b < 0x7F ? 0x40 : 0x41;
    if ((b >= 0x40 && b <= 0x7E) || (b >= 0x80 && b <= 0xFE)) {
      pointer = ((int64_t)lead - 0x81) * 190 + (b - offset);
    }
    uint32_t cp = pointer < 0 ? 0 : index_cp(e->table, e->first, e->count, pointer);
    if (cp) {
      emit(out, cp);
      return GO;
    }
    if (is_ascii(b)) {
      restore(q, &b, 1);
    }
    return ERROR;
  }
  if (is_ascii(b)) {
    emit(out, (uint32_t)b);
    return GO;
  }
  if (b == 0x80) {
    emit(out, 0x20AC);
    return GO;
  }
  if (b >= 0x81 && b <= 0xFE) {
    *first = (uint32_t)b;
    return GO;
  }

  return ERROR;
}

static int big5_byte(const TzCjkEncoding *e, uint32_t *s, Queue *q, TzBuf *out, int b) {
  uint32_t *leading = &s[0];
  if (b == END && *leading) {
    *leading = 0;
    return ERROR;
  }
  if (b == END) {
    return FINISHED;
  }
  if (*leading) {
    uint32_t lead = *leading;
    *leading = 0;
    int64_t pointer = -1;
    int offset = b < 0x7F ? 0x40 : 0x62;
    if ((b >= 0x40 && b <= 0x7E) || (b >= 0xA1 && b <= 0xFE)) {
      pointer = ((int64_t)lead - 0x81) * 157 + (b - offset);
    }
    static const struct {
      int32_t pointer;
      uint16_t first, second;
    } pairs[] = {{1133, 0x00CA, 0x0304},
                 {1135, 0x00CA, 0x030C},
                 {1164, 0x00EA, 0x0304},
                 {1166, 0x00EA, 0x030C}};
    for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; ++i) {
      if (pointer == pairs[i].pointer) {
        emit(out, pairs[i].first);
        emit(out, pairs[i].second);
        return GO;
      }
    }
    uint32_t cp = pointer < 0 ? 0 : index_cp(e->table, e->first, e->count, pointer);
    if (cp) {
      uint32_t k = (uint32_t)(pointer - e->first);
      if (e->astral[k >> 3] & (1u << (k & 7))) {
        cp += 0x20000;
      }
      emit(out, cp);
      return GO;
    }
    if (is_ascii(b)) {
      restore(q, &b, 1);
    }
    return ERROR;
  }
  if (is_ascii(b)) {
    emit(out, (uint32_t)b);
    return GO;
  }
  if (b >= 0x81 && b <= 0xFE) {
    *leading = (uint32_t)b;
    return GO;
  }

  return ERROR;
}

static int euc_jp_byte(const TzCjkEncoding *e, uint32_t *s, Queue *q, TzBuf *out, int b) {
  uint32_t *leading = &s[0], *jis0212 = &s[1];
  if (b == END && *leading) {
    *leading = 0;
    return ERROR;
  }
  if (b == END) {
    return FINISHED;
  }
  if (*leading == 0x8E && b >= 0xA1 && b <= 0xDF) {
    *leading = 0;
    emit(out, (uint32_t)(0xFF61 - 0xA1 + b));
    return GO;
  }
  if (*leading == 0x8F && b >= 0xA1 && b <= 0xFE) {
    *jis0212 = 1;
    *leading = (uint32_t)b;
    return GO;
  }
  if (*leading) {
    uint32_t lead = *leading;
    *leading = 0;
    uint32_t cp = 0;
    if (lead >= 0xA1 && lead <= 0xFE && b >= 0xA1 && b <= 0xFE) {
      int64_t pointer = ((int64_t)lead - 0xA1) * 94 + b - 0xA1;
      cp = *jis0212 ? index_cp(e->table2, e->first2, e->count2, pointer)
                    : index_cp(e->table, e->first, e->count, pointer);
    }
    *jis0212 = 0;
    if (cp) {
      emit(out, cp);
      return GO;
    }
    if (is_ascii(b)) {
      restore(q, &b, 1);
    }
    return ERROR;
  }
  if (is_ascii(b)) {
    emit(out, (uint32_t)b);
    return GO;
  }
  if (b == 0x8E || b == 0x8F || (b >= 0xA1 && b <= 0xFE)) {
    *leading = (uint32_t)b;
    return GO;
  }

  return ERROR;
}

enum {
  ISO_ASCII,
  ISO_ROMAN,
  ISO_KATAKANA,
  ISO_LEADING,
  ISO_TRAILING,
  ISO_ESCAPE_START,
  ISO_ESCAPE
};

static int iso_2022_jp_byte(const TzCjkEncoding *e, uint32_t *s, Queue *q, TzBuf *out, int b) {
  uint32_t *state = &s[0], *output_state = &s[1], *leading = &s[2], *output = &s[3];
  switch (*state) {
  case ISO_ASCII:
  case ISO_ROMAN:
  case ISO_KATAKANA:
  case ISO_LEADING:
    if (b == 0x1B) {
      *state = ISO_ESCAPE_START;
      return GO;
    }
    if (b == END) {
      return FINISHED;
    }
    *output = 0;
    if (*state == ISO_ROMAN && b == 0x5C) {
      emit(out, 0x00A5);
      return GO;
    }
    if (*state == ISO_ROMAN && b == 0x7E) {
      emit(out, 0x203E);
      return GO;
    }
    if ((*state == ISO_ASCII || *state == ISO_ROMAN) && b <= 0x7F && b != 0x0E && b != 0x0F) {
      emit(out, (uint32_t)b);
      return GO;
    }
    if (*state == ISO_KATAKANA && b >= 0x21 && b <= 0x5F) {
      emit(out, (uint32_t)(0xFF61 - 0x21 + b));
      return GO;
    }
    if (*state == ISO_LEADING && b >= 0x21 && b <= 0x7E) {
      *leading = (uint32_t)b;
      *state = ISO_TRAILING;
      return GO;
    }
    return ERROR;
  case ISO_TRAILING: {
    if (b == 0x1B) {
      *state = ISO_ESCAPE_START;
      return ERROR;
    }
    *state = ISO_LEADING;
    if (b >= 0x21 && b <= 0x7E) {
      uint32_t cp =
          index_cp(e->table, e->first, e->count, ((int64_t)*leading - 0x21) * 94 + b - 0x21);
      if (!cp) {
        return ERROR;
      }
      emit(out, cp);
      return GO;
    }
    return ERROR;
  }
  case ISO_ESCAPE_START:
    if (b == 0x24 || b == 0x28) {
      *leading = (uint32_t)b;
      *state = ISO_ESCAPE;
      return GO;
    }
    restore(q, &b, 1);
    *output = 0;
    *state = *output_state;
    return ERROR;
  default: {
    uint32_t lead = *leading;
    *leading = 0;
    int to = -1;
    if (lead == 0x28 && b == 0x42) {
      to = ISO_ASCII;
    }
    if (lead == 0x28 && b == 0x4A) {
      to = ISO_ROMAN;
    }
    if (lead == 0x28 && b == 0x49) {
      to = ISO_KATAKANA;
    }
    if (lead == 0x24 && (b == 0x40 || b == 0x42)) {
      to = ISO_LEADING;
    }
    if (to >= 0) {
      *state = *output_state = (uint32_t)to;
      uint32_t was = *output;
      *output = 1;
      return was ? ERROR : GO;
    }
    int bytes[2] = {(int)lead, b};
    restore(q, bytes, 2);
    *output = 0;
    *state = *output_state;
    return ERROR;
  }
  }
}

static int shift_jis_byte(const TzCjkEncoding *e, uint32_t *s, Queue *q, TzBuf *out, int b) {
  uint32_t *leading = &s[0];
  if (b == END && *leading) {
    *leading = 0;
    return ERROR;
  }
  if (b == END) {
    return FINISHED;
  }
  if (*leading) {
    uint32_t lead = *leading;
    *leading = 0;
    int64_t pointer = -1;
    int offset = b < 0x7F ? 0x40 : 0x41;
    int lead_offset = lead < 0xA0 ? 0x81 : 0xC1;
    if ((b >= 0x40 && b <= 0x7E) || (b >= 0x80 && b <= 0xFC)) {
      pointer = ((int64_t)lead - lead_offset) * 188 + b - offset;
    }
    if (pointer >= 8836 && pointer <= 10715) {
      emit(out, (uint32_t)(0xE000 - 8836 + pointer));
      return GO;
    }
    uint32_t cp = pointer < 0 ? 0 : index_cp(e->table, e->first, e->count, pointer);
    if (cp) {
      emit(out, cp);
      return GO;
    }
    if (is_ascii(b)) {
      restore(q, &b, 1);
    }
    return ERROR;
  }
  if (is_ascii(b) || b == 0x80) {
    emit(out, (uint32_t)b);
    return GO;
  }
  if (b >= 0xA1 && b <= 0xDF) {
    emit(out, (uint32_t)(0xFF61 - 0xA1 + b));
    return GO;
  }
  if ((b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC)) {
    *leading = (uint32_t)b;
    return GO;
  }

  return ERROR;
}

static int euc_kr_byte(const TzCjkEncoding *e, uint32_t *s, Queue *q, TzBuf *out, int b) {
  uint32_t *leading = &s[0];
  if (b == END && *leading) {
    *leading = 0;
    return ERROR;
  }
  if (b == END) {
    return FINISHED;
  }
  if (*leading) {
    uint32_t lead = *leading;
    *leading = 0;
    uint32_t cp = 0;
    if (b >= 0x41 && b <= 0xFE) {
      cp = index_cp(e->table, e->first, e->count, ((int64_t)lead - 0x81) * 190 + (b - 0x41));
    }
    if (cp) {
      emit(out, cp);
      return GO;
    }
    if (is_ascii(b)) {
      restore(q, &b, 1);
    }
    return ERROR;
  }
  if (is_ascii(b)) {
    emit(out, (uint32_t)b);
    return GO;
  }
  if (b >= 0x81 && b <= 0xFE) {
    *leading = (uint32_t)b;
    return GO;
  }

  return ERROR;
}

int32_t tz_cjk_decode(const TzCjkEncoding *e, uint32_t *state, TzBuf *out, const uint8_t *data,
                      int64_t length, int32_t stream, int32_t fatal) {
  typedef int (*Handler)(const TzCjkEncoding *, uint32_t *, Queue *, TzBuf *, int);
  static const Handler handlers[] = {gb18030_byte,     big5_byte,      euc_jp_byte,
                                     iso_2022_jp_byte, shift_jis_byte, euc_kr_byte};
  Handler handle = handlers[e->kind];
  Queue q = {{0}, 0, data, length, 0};
  for (;;) {
    int b = next_byte(&q, stream);
    if (b == -2) {
      return 1;
    }
    int result = handle(e, state, &q, out, b);
    if (result == FINISHED) {
      return 1;
    }
    if (result == ERROR) {
      if (fatal) {
        return 0;
      }
      tz_buf_push(out, 0xFFFD);
    }
  }
}

#define INDEX(name) TZ_CJK_##name##_FIRST, TZ_CJK_##name##_COUNT
#define RANGES tz_cjk_gb18030_ranges, TZ_CJK_GB18030_RANGES
const TzCjkEncoding tz_cjk_encoding_gbk = {
    "gbk",  TZ_CJK_KIND_GB18030, INDEX(GB18030), tz_cjk_gb18030, NULL, 0, 0, NULL,
    RANGES, tz_cjk_decode};
const TzCjkEncoding tz_cjk_encoding_gb18030 = {
    "gb18030", TZ_CJK_KIND_GB18030, INDEX(GB18030), tz_cjk_gb18030, NULL, 0, 0, NULL,
    RANGES,    tz_cjk_decode};
const TzCjkEncoding tz_cjk_encoding_big5 = {
    "big5", TZ_CJK_KIND_BIG5, INDEX(BIG5), tz_cjk_big5, tz_cjk_big5_astral, 0, 0, NULL, NULL,
    0,      tz_cjk_decode};
const TzCjkEncoding tz_cjk_encoding_euc_jp = {
    "euc-jp", TZ_CJK_KIND_EUC_JP, INDEX(JIS0208), tz_cjk_jis0208,
    NULL,     INDEX(JIS0212),     tz_cjk_jis0212, NULL,
    0,        tz_cjk_decode};
const TzCjkEncoding tz_cjk_encoding_iso_2022_jp = {"iso-2022-jp",
                                                   TZ_CJK_KIND_ISO_2022_JP,
                                                   INDEX(JIS0208),
                                                   tz_cjk_jis0208,
                                                   NULL,
                                                   0,
                                                   0,
                                                   NULL,
                                                   NULL,
                                                   0,
                                                   tz_cjk_decode};
const TzCjkEncoding tz_cjk_encoding_shift_jis = {
    "shift_jis", TZ_CJK_KIND_SHIFT_JIS, INDEX(JIS0208), tz_cjk_jis0208, NULL, 0, 0, NULL, NULL,
    0,           tz_cjk_decode};
const TzCjkEncoding tz_cjk_encoding_euc_kr = {
    "euc-kr", TZ_CJK_KIND_EUC_KR, INDEX(EUC_KR), tz_cjk_euc_kr, NULL, 0, 0, NULL, NULL,
    0,        tz_cjk_decode};
#undef RANGES
#undef INDEX
