#include "runtime/library/json/json_stringify/json_stringify.h"
#include "runtime/library/json/json/json.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/array/array.h"
#include "runtime/core/error/error.h"
#include "runtime/core/map/map.h"
#include "runtime/core/output/output.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include "runtime/core/unknown/unknown.h"
#include "runtime/library/bytes/bytes/bytes.h"
#include "runtime/library/date/date/date.h"
#include "runtime/library/decimal/decimal/decimal.h"
#include <math.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STR_LEN_MASK ((1ull << 30) - 1)
#define STR_TWO_BYTE (1ull << 61)
#define STR_ROPE (1ull << 62)
#define STR_HEAP (1ull << 63)

#define ONES 0x0101010101010101ull
#define HIGHS 0x8080808080808080ull

#define SCRATCH_KEEP (256u * 1024u)

TZ_THREAD_LOCAL uint8_t *tz_json_scratch;
TZ_THREAD_LOCAL size_t tz_json_scratch_capacity;

TZ_THREAD_LOCAL TzJsonOut tz_json_out_state;

typedef TzJsonOut Out;

typedef struct {
  Out *out;
  TzBuf gap;
  TzBuf indent;
  const void **open;
  const void *open_inline[32];
  size_t depth, capacity;
  int owns;
  void *fail[5];
} Writer;

static void out_init(Out *o) {
  o->data = tz_json_scratch;
  o->capacity = tz_json_scratch_capacity;
  o->length = 0;
  o->two = 0;
  o->high = 0;
}

static void out_release(Out *o) {
  if (o->two) {
    free(o->data);
  }
  if (tz_json_scratch_capacity > SCRATCH_KEEP) {
    free(tz_json_scratch);
    tz_json_scratch = NULL;
    tz_json_scratch_capacity = 0;
  }
  o->data = NULL;
  o->capacity = 0;
  o->length = 0;
}

void tz_json_out_grow(Out *o, size_t n) {
  size_t capacity = o->capacity * 2;
  if (capacity < o->length + n) {
    capacity = o->length + n;
  }
  if (capacity < 1024) {
    capacity = 1024;
  }
  if (o->two) {
    o->data = (uint8_t *)realloc(o->data, capacity * 2);
  } else {
    tz_json_scratch = (uint8_t *)realloc(tz_json_scratch, capacity);
    tz_json_scratch_capacity = tz_json_scratch ? capacity : 0;
    o->data = tz_json_scratch;
  }
  if (!o->data) {
    tz_panicf("out of memory: JSON.stringify needs %llu bytes",
              (unsigned long long)(capacity * (o->two ? 2 : 1)));
  }
  o->capacity = capacity;
}

static inline void out_need(Out *o, size_t n) {
  if (o->capacity - o->length < n) {
    tz_json_out_grow(o, n);
  }
}

static TZ_NOINLINE void out_widen(Out *o) {
  size_t capacity = o->capacity < 512 ? 512 : o->capacity;
  uint16_t *units = (uint16_t *)malloc(capacity * 2);
  if (!units) {
    tz_panicf("out of memory: JSON.stringify needs %llu bytes", (unsigned long long)(capacity * 2));
  }
  for (size_t i = 0; i < o->length; ++i) {
    units[i] = o->data[i];
  }
  o->data = (uint8_t *)units;
  o->capacity = capacity;
  o->two = 1;
}

static inline uint16_t *out_units(Out *o) { return (uint16_t *)o->data + o->length; }

static inline void out_char(Out *o, char c) {
  out_need(o, 1);
  if (o->two) {
    *out_units(o) = (uint8_t)c;
  } else {
    o->data[o->length] = (uint8_t)c;
  }
  ++o->length;
}

static inline void out_ascii(Out *o, const char *text, size_t n) {
  out_need(o, n);
  if (o->two) {
    uint16_t *d = out_units(o);
    for (size_t i = 0; i < n; ++i) {
      d[i] = (uint8_t)text[i];
    }
  } else {
    memcpy(o->data + o->length, text, n);
  }
  o->length += n;
}

static void out_utf16(Out *o, const uint16_t *units, size_t n) {
  if (!o->two) {
    for (size_t i = 0; i < n; ++i) {
      if (units[i] > 0xFF) {
        out_widen(o);
        break;
      }
    }
  }
  out_need(o, n);
  if (o->two) {
    memcpy(out_units(o), units, n * 2);
  } else {
    uint8_t *d = o->data + o->length;
    for (size_t i = 0; i < n; ++i) {
      d[i] = (uint8_t)units[i];
      o->high |= units[i];
    }
  }
  o->length += n;
}

static const char hex_digits[] = "0123456789abcdef";

static size_t escape_of(uint32_t c, char *text) {
  text[0] = '\\';
  switch (c) {
  case '"':
    text[1] = '"';
    return 2;
  case '\\':
    text[1] = '\\';
    return 2;
  case '\b':
    text[1] = 'b';
    return 2;
  case '\f':
    text[1] = 'f';
    return 2;
  case '\n':
    text[1] = 'n';
    return 2;
  case '\r':
    text[1] = 'r';
    return 2;
  case '\t':
    text[1] = 't';
    return 2;
  default:
    text[1] = 'u';
    text[2] = hex_digits[(c >> 12) & 15];
    text[3] = hex_digits[(c >> 8) & 15];
    text[4] = hex_digits[(c >> 4) & 15];
    text[5] = hex_digits[c & 15];
    return 6;
  }
}

static inline int byte_plain(uint8_t c) { return c >= 0x20 && c != '"' && c != '\\'; }

static inline uint64_t dirty_lanes(uint64_t x) {
  uint64_t low = (x - ONES * 0x20) & ~x;
  uint64_t q = x ^ (ONES * '"');
  uint64_t quote = (q - ONES) & ~q;
  uint64_t s = x ^ (ONES * '\\');
  uint64_t slash = (s - ONES) & ~s;

  return (low | quote | slash) & HIGHS;
}

static TZ_NOINLINE void quote_bytes_rest(Out *o, const uint8_t *p, size_t n) {
  out_need(o, n * 6 + 1);
  if (o->two) {
    uint16_t *d = out_units(o);
    for (size_t i = 0; i < n; ++i) {
      uint8_t c = p[i];
      if (byte_plain(c)) {
        *d++ = c;
        continue;
      }
      char text[6];
      size_t k = escape_of(c, text);
      for (size_t j = 0; j < k; ++j) {
        d[j] = (uint8_t)text[j];
      }
      d += k;
    }
    o->length = (size_t)(d - (uint16_t *)o->data);
    return;
  }
  uint8_t *d = o->data + o->length;
  uint64_t high = 0;
  for (size_t i = 0; i < n; ++i) {
    uint8_t c = p[i];
    if (byte_plain(c)) {
      *d++ = c;
      high |= c;
      continue;
    }
    d += escape_of(c, (char *)d);
  }
  o->high |= high;
  o->length = (size_t)(d - o->data);
}

static void quote_bytes(Out *o, const uint8_t *p, size_t n) {
  if (o->two) {
    out_char(o, '"');
    quote_bytes_rest(o, p, n);
    out_char(o, '"');
    return;
  }
  out_need(o, n + 2);
  uint8_t *d = o->data + o->length + 1;
  o->data[o->length] = '"';
  size_t i = 0;
  uint64_t high = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t x;
    memcpy(&x, p + i, 8);
    if (dirty_lanes(x)) {
      break;
    }
    memcpy(d + i, &x, 8);
    high |= x;
  }
  if (i < n && n >= 8 && i + 8 > n) {
    uint64_t x;
    memcpy(&x, p + n - 8, 8);
    if (!dirty_lanes(x)) {
      memcpy(d + n - 8, &x, 8);
      high |= x;
      i = n;
    }
  }
  o->high |= high;
  o->length += 1 + i;
  if (i < n) {
    quote_bytes_rest(o, p + i, n - i);
  }
  out_char(o, '"');
}

static void quote_inline(Out *o, uint64_t bits, uint32_t n) {
  uint64_t mask = n >= 8 ? ~0ull : ((1ull << (8 * n)) - 1);
  uint64_t x = (bits & mask) | ((ONES * 'a') & ~mask);
  if (o->two || dirty_lanes(x)) {
    uint8_t bytes[8];
    memcpy(bytes, &bits, 8);
    quote_bytes(o, bytes, n);
    return;
  }
  out_need(o, 10);
  uint8_t *d = o->data + o->length;
  d[0] = '"';
  memcpy(d + 1, &bits, 8);
  d[1 + n] = '"';
  o->length += n + 2;
  o->high |= bits & mask;
}

static TZ_NOINLINE void quote_units(Out *o, const uint16_t *p, size_t n) {
  if (!o->two) {
    out_widen(o);
  }
  out_need(o, n * 6 + 2);
  uint16_t *d = out_units(o);
  *d++ = '"';
  for (size_t i = 0; i < n; ++i) {
    uint16_t c = p[i];
    if (c >= 0x20 && c != '"' && c != '\\' && (c < 0xD800 || c > 0xDFFF)) {
      *d++ = c;
      continue;
    }
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && p[i + 1] >= 0xDC00 && p[i + 1] <= 0xDFFF) {
      *d++ = c;
      *d++ = p[++i];
      continue;
    }
    char text[6];
    size_t k = escape_of(c, text);
    for (size_t j = 0; j < k; ++j) {
      d[j] = (uint8_t)text[j];
    }
    d += k;
  }
  *d++ = '"';
  o->length = (size_t)(d - (uint16_t *)o->data);
}

static void quote(Out *o, TzStr s) {
  uint64_t meta = s.meta;
  if ((meta & (STR_ROPE | STR_HEAP | STR_TWO_BYTE)) == STR_ROPE) {
    quote_inline(o, (uint64_t)(uintptr_t)s.obj, (uint32_t)(meta & STR_LEN_MASK));
    return;
  }
  uint32_t n;
  int two;
  const void *units = tz_str_units(&s, &n, &two);
  if (two) {
    quote_units(o, (const uint16_t *)units, n);
  } else {
    quote_bytes(o, (const uint8_t *)units, n);
  }
}

static void quote_name(Out *o, const char *name, size_t n) {
  int plain = 1;
  for (size_t i = 0; i < n; ++i) {
    uint8_t c = (uint8_t)name[i];
    if (c >= 0x80 || !byte_plain(c)) {
      plain = 0;
      break;
    }
  }
  if (plain) {
    out_need(o, n + 2);
    if (o->two) {
      uint16_t *d = out_units(o);
      d[0] = '"';
      for (size_t i = 0; i < n; ++i) {
        d[1 + i] = (uint8_t)name[i];
      }
      d[1 + n] = '"';
    } else {
      uint8_t *d = o->data + o->length;
      d[0] = '"';
      memcpy(d + 1, name, n);
      d[1 + n] = '"';
    }
    o->length += n + 2;
    return;
  }
  TzBuf key;
  tz_buf_init(&key);
  tz_buf_utf8(&key, name, n);
  TzStr s = tz_buf_take(&key);
  quote(o, s);
  tz_str_release_value(s);
}

static inline int name_words(const char *name, size_t n, uint64_t *first, uint64_t *last) {
  if (n >= 8) {
    if (n > 16) {
      return 0;
    }
    memcpy(first, name, 8);
    memcpy(last, name + n - 8, 8);
  } else {
    uint64_t x = 0;
    if (n >= 4) {
      uint32_t head, tail;
      memcpy(&head, name, 4);
      memcpy(&tail, name + n - 4, 4);
      x = (uint64_t)head | ((uint64_t)tail << (8 * (n - 4)));
    } else {
      for (size_t i = 0; i < n; ++i) {
        x |= (uint64_t)(uint8_t)name[i] << (8 * i);
      }
    }
    uint64_t mask = (1ull << (8 * n)) - 1;
    x = (x & mask) | ((ONES * 'a') & ~mask);
    *first = x;
    *last = x;
  }

  return !(dirty_lanes(*first) | dirty_lanes(*last) | ((*first | *last) & HIGHS));
}

static void separate(Writer *w, uint32_t written);
static void colon(Writer *w);

static inline void write_key(Writer *w, uint32_t written, const char *name, size_t n) {
  Out *o = w->out;
  uint64_t first, last;
  if (w->gap.length || o->two || n == 0 || !name_words(name, n, &first, &last)) {
    separate(w, written);
    quote_name(o, name, n);
    colon(w);
    return;
  }
  out_need(o, n + 12);
  uint8_t *d = o->data + o->length;
  size_t k = 0;
  if (written) {
    d[k++] = ',';
  }
  d[k] = '"';
  memcpy(d + k + 1, &first, 8);
  if (n > 8) {
    memcpy(d + k + 1 + n - 8, &last, 8);
  }
  d[k + 1 + n] = '"';
  d[k + 2 + n] = ':';
  o->length += k + n + 3;
}

static void writer_release(Writer *w) {
  tz_buf_free(&w->gap);
  tz_buf_free(&w->indent);
  if (w->open != w->open_inline) {
    tz_free((void *)w->open);
  }
}

static void enter(Writer *w, const void *container) {
  for (size_t i = 0; i < w->depth; ++i) {
    if (w->open[i] == container) {
      writer_release(w);
      if (w->owns) {
        out_release(w->out);
      }
      TzBuf b;
      tz_buf_init(&b);
      tz_buf_ascii(&b, "Converting circular structure to JSON");
      tz_json_fail("ERR_CIRCULAR_STRUCTURE", &b);
      __builtin_longjmp(w->fail, 1);
    }
  }
  if (w->depth == w->capacity) {
    size_t capacity = w->capacity * 2;
    const void **open = (const void **)tz_alloc(capacity * sizeof(void *));
    memcpy((void *)open, w->open, w->depth * sizeof(void *));
    if (w->open != w->open_inline) {
      tz_free((void *)w->open);
    }
    w->open = open;
    w->capacity = capacity;
  }
  w->open[w->depth++] = container;
  if (w->gap.length) {
    tz_buf_units(&w->indent, w->gap.data, w->gap.length);
  }
}

static inline void leave(Writer *w) {
  --w->depth;
  w->indent.length -= w->gap.length;
}

static void separate(Writer *w, uint32_t written) {
  if (written) {
    out_char(w->out, ',');
  }
  if (w->gap.length) {
    out_char(w->out, '\n');
    out_utf16(w->out, w->indent.data, w->indent.length);
  }
}

static void colon(Writer *w) {
  if (w->gap.length) {
    out_ascii(w->out, ": ", 2);
  } else {
    out_char(w->out, ':');
  }
}

static void close_with(Writer *w, uint32_t written, char bracket) {
  if (written && w->gap.length) {
    out_char(w->out, '\n');
    out_utf16(w->out, w->indent.data, w->indent.length);
  }
  out_char(w->out, bracket);
}

static void write_number(Out *o, double value) {
  if (!isfinite(value)) {
    out_ascii(o, "null", 4);
    return;
  }
  char text[32];
  out_ascii(o, text, tz_format_f64(value == 0 ? 0 : value, text));
}

static void write_integer(Out *o, uint32_t kind, int64_t value) {
  char text[24];
  int negative = kind != TZ_T_UINT64 && value < 0;
  uint64_t magnitude = negative ? 0 - (uint64_t)value : (uint64_t)value;
  char *p = text + sizeof text;
  do {
    *--p = (char)('0' + magnitude % 10);
    magnitude /= 10;
  } while (magnitude != 0);
  if (negative) {
    *--p = '-';
  }
  out_ascii(o, p, (size_t)(text + sizeof text - p));
}

static void write_bool(Out *o, int value) {
  if (value) {
    out_ascii(o, "true", 4);
  } else {
    out_ascii(o, "false", 5);
  }
}

static int write_value(Writer *w, const TzType *type, const void *value);

static int write_json(Writer *w, const TzJson *j) {
  if (!j) {
    return 0;
  }
  switch (j->kind) {
  case TZ_JSON_NULL:
    out_ascii(w->out, "null", 4);
    return 1;
  case TZ_JSON_BOOL:
    write_bool(w->out, j->u.boolean);
    return 1;
  case TZ_JSON_NUMBER:
    write_number(w->out, j->u.number.value);
    return 1;
  case TZ_JSON_STRING:
    quote(w->out, j->u.string);
    return 1;
  case TZ_JSON_ARRAY:
    return write_value(w, NULL, &j->u.array);
  default:
    break;
  }
  enter(w, j);
  out_char(w->out, '{');
  uint32_t *order = tz_json_key_order(j);
  uint32_t written = 0;
  for (uint32_t k = 0; k < j->u.object.count; ++k) {
    const TzJsonEntry *e = &j->u.object.entries[order ? order[k] : k];
    separate(w, written++);
    quote(w->out, e->key);
    colon(w);
    write_json(w, e->value);
  }
  tz_free(order);
  leave(w);
  close_with(w, written, '}');

  return 1;
}

static int has_json(const TzType *type, const void *value) {
  type = tz_resolve(type, &value);
  switch (type->kind) {
  case TZ_T_UNDEFINED:
  case TZ_T_FUNCTION:
    return 0;
  case TZ_T_OPTIONAL:
    return tz_optional_has(type, value) && has_json(type->inner, value);
  case TZ_T_JSON:
    return *(TzJson *const *)value != NULL;
  case TZ_T_UNKNOWN: {
    const TzInstance *u = *(const TzInstance *const *)value;
    if (!u) {
      return 0;
    }
    if (u->layout->flags & TZ_LAYOUT_BOX) {
      return has_json(((const TzBox *)u)->type, TZ_BOX_VALUE(u));
    }
    return 1;
  }
  default:
    return 1;
  }
}

static void write_elements(Writer *w, const TzType *type, const char *data, uint32_t count,
                           uint32_t stride) {
  if (type && type->kind == TZ_T_STRING && !w->gap.length) {
    for (uint32_t i = 0; i < count; ++i) {
      if (i) {
        out_char(w->out, ',');
      }
      quote(w->out, *(const TzStr *)(data + (size_t)i * stride));
    }
    return;
  }
  for (uint32_t i = 0; i < count; ++i) {
    separate(w, i);
    const void *value = data + (size_t)i * stride;
    if (!(type ? write_value(w, type, value) : write_json(w, *(TzJson *const *)value))) {
      out_ascii(w->out, "null", 4);
    }
  }
}

static TZ_NOINLINE int write_decimal(Out *out, const void *value) {
  TzBuf text;
  tz_buf_init(&text);
  tz_buf_decimal(&text, *(const TzDec *)value);
  TzStr s = tz_buf_take(&text);
  quote(out, s);
  tz_str_release_value(s);

  return 1;
}

static TZ_NOINLINE int write_unknown(Writer *w, const void *value) {
  static const TzType object_type = {TZ_T_OBJECT, sizeof(void *), NULL};
  const TzInstance *u = *(const TzInstance *const *)value;
  if (!u) {
    return 0;
  }
  if (u->layout->flags & TZ_LAYOUT_BOX) {
    return write_value(w, ((const TzBox *)u)->type, TZ_BOX_VALUE(u));
  }

  return write_value(w, &object_type, value);
}

static TZ_NOINLINE int write_result(Writer *w, const TzType *type, const void *value) {
  Out *out = w->out;
  const TzResultType *result = (const TzResultType *)type;
  const char *object = (const char *)value;
  int ok = object[result->ok_offset] != 0;
  const TzType *half = ok ? type->inner : result->error;
  const void *held = object + (ok ? result->value_offset : result->error_offset);
  enter(w, value);
  out_char(out, '{');
  uint32_t written = 0;
  if (has_json(half, held)) {
    separate(w, written++);
    if (ok) {
      out_ascii(out, "\"value\"", 7);
    } else {
      out_ascii(out, "\"error\"", 7);
    }
    colon(w);
    write_value(w, half, held);
  }
  leave(w);
  close_with(w, written, '}');

  return 1;
}

static TZ_NOINLINE int write_date(Out *out, const TzInstance *object) {
  TzStr text;
  tz_date_iso(&text, ((const TzDate *)object)->time);
  quote(out, text);
  tz_str_release_value(text);

  return 1;
}

static TZ_NOINLINE int write_bytes(Writer *w, const TzInstance *object) {
  Out *out = w->out;
  const TzBytes *bytes = (const TzBytes *)object;
  enter(w, object);
  out_char(out, '{');
  int buffer = tz_bytes_kind(object) == TZ_BYTES_BUFFER;
  if (buffer) {
    separate(w, 0);
    out_ascii(out, "\"type\"", 6);
    colon(w);
    out_ascii(out, "\"Buffer\"", 8);
    separate(w, 1);
    out_ascii(out, "\"data\"", 6);
    colon(w);
    enter(w, &bytes->data);
    out_char(out, '[');
  }
  for (int32_t i = 0; i < bytes->length; ++i) {
    separate(w, (uint32_t)i);
    if (!buffer) {
      char key[16];
      int n = snprintf(key, sizeof key, "\"%d\"", i);
      out_ascii(out, key, (size_t)n);
      colon(w);
    }
    write_number(out, tz_bytes_get(bytes, i));
  }
  if (buffer) {
    leave(w);
    close_with(w, (uint32_t)bytes->length, ']');
  }
  leave(w);
  close_with(w, buffer ? 2 : (uint32_t)bytes->length, '}');

  return 1;
}

static TZ_NOINLINE int write_dict(Writer *w, const TzInstance *object) {
  Out *out = w->out;
  const TzMap *m = (const TzMap *)object;
  const TzMapLayout *l = m->layout;
  enter(w, object);
  out_char(out, '{');
  uint32_t written = 0, at = 0;
  if (l->kind == TZ_MAP_RECORD) {
    for (const char *e; (e = (const char *)tz_map_next(m, &at));) {
      if (!has_json(l->value, e + l->value_offset)) {
        continue;
      }
      separate(w, written++);
      quote(out, *(const TzStr *)(e + l->key_offset));
      colon(w);
      write_value(w, l->value, e + l->value_offset);
    }
  }
  leave(w);
  close_with(w, written, '}');

  return 1;
}

static TZ_NOINLINE int write_tuple(Writer *w, const TzInstance *object) {
  const TzLayout *layout = object->layout;
  const char *fields = (const char *)object;
  enter(w, object);
  out_char(w->out, '[');
  for (uint32_t i = 0; i < layout->count; ++i) {
    separate(w, i);
    if (!write_value(w, layout->fields[i].type, fields + layout->fields[i].offset)) {
      out_ascii(w->out, "null", 4);
    }
  }
  leave(w);
  close_with(w, layout->count, ']');

  return 1;
}

static int write_object(Writer *w, const TzInstance *object) {
  const TzLayout *layout = object->layout;
  uint32_t flags = layout->flags;
  const char *fields = (const char *)object;
  if (flags & (TZ_LAYOUT_BOX | TZ_LAYOUT_CLASS | TZ_LAYOUT_MATCH | TZ_LAYOUT_DATE |
               TZ_LAYOUT_BYTES | TZ_LAYOUT_DICT | TZ_LAYOUT_TUPLE)) {
    if (flags & TZ_LAYOUT_BOX) {
      return write_value(w, ((const TzBox *)object)->type, TZ_BOX_VALUE(object));
    }
    if (flags & TZ_LAYOUT_CLASS) {
      return 0;
    }
    if (flags & TZ_LAYOUT_MATCH) {
      return write_value(w, layout->fields[0].type, fields + layout->fields[0].offset);
    }
    if (flags & TZ_LAYOUT_DATE) {
      return write_date(w->out, object);
    }
    if ((flags & TZ_LAYOUT_BYTES) && tz_bytes_kind(object) <= TZ_BYTES_BUFFER) {
      return write_bytes(w, object);
    }
    if (flags & TZ_LAYOUT_DICT) {
      return write_dict(w, object);
    }
    if (flags & TZ_LAYOUT_TUPLE) {
      return write_tuple(w, object);
    }
  }
  enter(w, object);
  out_char(w->out, '{');
  uint32_t written = 0;
  for (uint32_t i = 0; i < layout->count; ++i) {
    const TzField *field = &layout->fields[i];
    if (field->offset == TZ_FIELD_METHOD || field->offset == TZ_FIELD_GETTER ||
        tz_field_is_symbol(layout, field)) {
      continue;
    }
    const void *at = fields + field->offset;
    if (field->type->kind == TZ_T_STRING) {
      write_key(w, written++, field->name, field->name_length);
      quote(w->out, *(const TzStr *)at);
      continue;
    }
    if (!has_json(field->type, at)) {
      continue;
    }
    write_key(w, written++, field->name, field->name_length);
    write_value(w, field->type, at);
  }
  leave(w);
  close_with(w, written, '}');

  return 1;
}

static int write_value(Writer *w, const TzType *type, const void *value) {
  Out *out = w->out;
  if (!type) {
    const TzArray *a = *(TzArray *const *)value;
    enter(w, a);
    out_char(out, '[');
    write_elements(w, NULL, a->data, a->length, sizeof(TzJson *));
    leave(w);
    close_with(w, a->length, ']');
    return 1;
  }
  type = tz_resolve(type, &value);
  switch (type->kind) {
  case TZ_T_STRING:
    quote(out, *(const TzStr *)value);
    return 1;
  case TZ_T_INT8:
  case TZ_T_INT16:
  case TZ_T_INT32:
  case TZ_T_INT64:
  case TZ_T_UINT8:
  case TZ_T_UINT16:
  case TZ_T_UINT32:
  case TZ_T_UINT64:
    write_integer(out, type->kind, tz_load_integer(type->kind, value));
    return 1;
  case TZ_T_DOUBLE:
    write_number(out, *(const double *)value);
    return 1;
  case TZ_T_BOOL:
    write_bool(out, *(const uint8_t *)value);
    return 1;
  case TZ_T_DECIMAL:
    return write_decimal(out, value);
  case TZ_T_JSON:
    return write_json(w, *(TzJson *const *)value);
  case TZ_T_UNKNOWN:
    return write_unknown(w, value);
  case TZ_T_OPTIONAL:
    return tz_optional_has(type, value) && write_value(w, type->inner, value);
  case TZ_T_ARRAY: {
    const TzArray *a = *(TzArray *const *)value;
    enter(w, a);
    out_char(out, '[');
    write_elements(w, a->type, a->data, a->length, a->type->size);
    leave(w);
    close_with(w, a->length, ']');
    return 1;
  }
  case TZ_T_RESULT:
    return write_result(w, type, value);
  case TZ_T_OBJECT:
    return write_object(w, *(TzInstance *const *)value);
  default:
    return 0;
  }
}

static void writer_init(Writer *w, Out *out) {
  memset(w, 0, sizeof *w);
  w->out = out;
  w->open = w->open_inline;
  w->capacity = sizeof w->open_inline / sizeof w->open_inline[0];
  tz_buf_init(&w->gap);
  tz_buf_init(&w->indent);
}

static TzStr out_text(Out *o) {
  if (o->two) {
    return tz_str_from_utf16((const uint16_t *)o->data, o->length);
  }

  return tz_str_from_latin1(o->data, o->length, (o->high & HIGHS) == 0);
}

int32_t tz_json_stringify(TzStr *out, const TzType *type, const void *value, int32_t space_kind,
                          double space_number, void *space_obj, uint64_t space_meta) {
  Writer w;
  Out text;
  out_init(&text);
  writer_init(&w, &text);
  w.owns = 1;
  *out = tz_str_from_ascii("", 0);
  if (__builtin_setjmp(w.fail)) {
    return 0;
  }
  if (space_kind == 1) {
    double n = space_number < 10 ? floor(space_number) : 10;
    for (int i = 0; i < (int)n; ++i) {
      tz_buf_push(&w.gap, ' ');
    }
  } else if (space_kind == 2) {
    TzStr s = {space_obj, space_meta};
    tz_buf_str(&w.gap, s);
    if (w.gap.length > 10) {
      w.gap.length = 10;
    }
  }
  int written = write_value(&w, type, value);
  if (written) {
    *out = out_text(&text);
  }
  writer_release(&w);
  out_release(&text);

  return written;
}

void *tz_json_out_begin(void) {
  out_init(&tz_json_out_state);

  return &tz_json_out_state;
}

void tz_json_out_end(void *out, TzStr *text) {
  Out *o = (Out *)out;
  *text = out_text(o);
  out_release(o);
}

TZ_NOINLINE void tz_json_out_raw_slow(Out *o, const char *text, int64_t length) {
  out_ascii(o, text, (size_t)length);
}

TZ_HOT void tz_json_out_raw(void *out, const char *text, int64_t length) {
  Out *o = (Out *)out;
  size_t n = (size_t)length;
  if (o->two || o->capacity - o->length < n) {
    tz_json_out_raw_slow(o, text, length);
    return;
  }
  memcpy(o->data + o->length, text, n);
  o->length += n;
}

TZ_NOINLINE void tz_json_out_str_slow(Out *o, void *obj, uint64_t meta) {
  TzStr s = {obj, meta};
  quote(o, s);
}

TZ_HOT __attribute__((always_inline)) void tz_json_out_str(void *out, void *obj, uint64_t meta) {
  Out *o = (Out *)out;
  if ((meta & (STR_ROPE | STR_HEAP | STR_TWO_BYTE)) == STR_ROPE && !o->two &&
      o->capacity - o->length >= 10) {
    uint32_t n = (uint32_t)(meta & STR_LEN_MASK);
    uint64_t bits = (uint64_t)(uintptr_t)obj;
    uint64_t mask = n >= 8 ? ~0ull : ((1ull << (8 * n)) - 1);
    if (!dirty_lanes((bits & mask) | ((ONES * 'a') & ~mask))) {
      uint8_t *d = o->data + o->length;
      d[0] = '"';
      memcpy(d + 1, &bits, 8);
      d[1 + n] = '"';
      o->length += n + 2;
      o->high |= bits & mask;
      return;
    }
  }
  tz_json_out_str_slow(o, obj, meta);
}

void tz_json_out_int(void *out, int64_t value) { write_integer((Out *)out, TZ_T_INT64, value); }

void tz_json_out_uint(void *out, uint64_t value) {
  write_integer((Out *)out, TZ_T_UINT64, (int64_t)value);
}

void tz_json_out_double(void *out, double value) { write_number((Out *)out, value); }

int32_t tz_json_out_value(void *out, const TzType *type, const void *value) {
  Writer w;
  writer_init(&w, (Out *)out);
  if (__builtin_setjmp(w.fail)) {
    tz_panicf("internal error: JSON.stringify met a cycle in a value of a type without cycles");
  }
  int written = write_value(&w, type, value);
  writer_release(&w);

  return written;
}
