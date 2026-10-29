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

typedef struct {
  TzBuf out;
  TzBuf gap;
  TzBuf indent;
  const void **open;
  size_t depth, capacity;
  void *fail[5];
} Writer;

static void writer_release(Writer *w) {
  tz_buf_free(&w->out);
  tz_buf_free(&w->gap);
  tz_buf_free(&w->indent);
  tz_free((void *)w->open);
}

static void enter(Writer *w, const void *container) {
  for (size_t i = 0; i < w->depth; ++i) {
    if (w->open[i] == container) {
      writer_release(w);
      TzBuf b;
      tz_buf_init(&b);
      tz_buf_ascii(&b, "Converting circular structure to JSON");
      tz_json_fail("ERR_CIRCULAR_STRUCTURE", &b, NULL);
      __builtin_longjmp(w->fail, 1);
    }
  }
  if (w->depth == w->capacity) {
    size_t capacity = w->capacity * 2 + 16;
    const void **open = (const void **)tz_alloc(capacity * sizeof(void *));
    if (w->depth) {
      memcpy((void *)open, w->open, w->depth * sizeof(void *));
    }
    tz_free((void *)w->open);
    w->open = open;
    w->capacity = capacity;
  }
  w->open[w->depth++] = container;
  tz_buf_units(&w->indent, w->gap.data, w->gap.length);
}

static void leave(Writer *w) {
  --w->depth;
  w->indent.length -= w->gap.length;
}

static void separate(Writer *w, uint32_t written) {
  if (written) {
    tz_buf_push(&w->out, ',');
  }
  if (w->gap.length) {
    tz_buf_push(&w->out, '\n');
    tz_buf_units(&w->out, w->indent.data, w->indent.length);
  }
}

static void close_with(Writer *w, uint32_t written, char bracket) {
  if (written && w->gap.length) {
    tz_buf_push(&w->out, '\n');
    tz_buf_units(&w->out, w->indent.data, w->indent.length);
  }
  tz_buf_push(&w->out, (uint16_t)bracket);
}

static const char hex_digits[] = "0123456789abcdef";

void tz_json_quote(TzBuf *out, TzStr s) {
  View v;
  tz_json_view_init(&v, s);
  tz_buf_push(out, '"');
  for (uint32_t i = 0; i < v.length; ++i) {
    uint16_t c = tz_json_view_at(&v, i);
    switch (c) {
    case '"':
      tz_buf_ascii(out, "\\\"");
      continue;
    case '\\':
      tz_buf_ascii(out, "\\\\");
      continue;
    case '\b':
      tz_buf_ascii(out, "\\b");
      continue;
    case '\f':
      tz_buf_ascii(out, "\\f");
      continue;
    case '\n':
      tz_buf_ascii(out, "\\n");
      continue;
    case '\r':
      tz_buf_ascii(out, "\\r");
      continue;
    case '\t':
      tz_buf_ascii(out, "\\t");
      continue;
    default:
      break;
    }
    int lone = 0;
    if (c >= 0xD800 && c <= 0xDBFF) {
      if (i + 1 < v.length && tz_json_view_at(&v, i + 1) >= 0xDC00 &&
          tz_json_view_at(&v, i + 1) <= 0xDFFF) {
        tz_buf_push(out, c);
        tz_buf_push(out, tz_json_view_at(&v, ++i));
        continue;
      }
      lone = 1;
    } else if (c >= 0xDC00 && c <= 0xDFFF) {
      lone = 1;
    }
    if (c < 0x20 || lone) {
      char text[7] = {'\\',
                      'u',
                      hex_digits[c >> 12],
                      hex_digits[(c >> 8) & 15],
                      hex_digits[(c >> 4) & 15],
                      hex_digits[c & 15],
                      0};
      tz_buf_ascii_n(out, text, 6);
    } else {
      tz_buf_push(out, c);
    }
  }
  tz_buf_push(out, '"');
}

static void write_number(TzBuf *out, double value) {
  if (!isfinite(value)) {
    tz_buf_ascii(out, "null");
    return;
  }
  char text[32];
  tz_buf_ascii_n(out, text, tz_format_f64(value == 0 ? 0 : value, text));
}

static int write_value(Writer *w, const TzType *type, const void *value);

static int write_json(Writer *w, const TzJson *j) {
  if (!j) {
    return 0;
  }
  switch (j->kind) {
  case TZ_JSON_NULL:
    tz_buf_ascii(&w->out, "null");
    return 1;
  case TZ_JSON_BOOL:
    tz_buf_ascii(&w->out, j->u.boolean ? "true" : "false");
    return 1;
  case TZ_JSON_NUMBER:
    write_number(&w->out, j->u.number.value);
    return 1;
  case TZ_JSON_STRING:
    tz_json_quote(&w->out, j->u.string);
    return 1;
  case TZ_JSON_ARRAY:
    return write_value(w, NULL, &j->u.array);
  default:
    break;
  }
  enter(w, j);
  tz_buf_push(&w->out, '{');
  uint32_t *order = tz_json_key_order(j);
  uint32_t written = 0;
  for (uint32_t k = 0; k < j->u.object.count; ++k) {
    const TzJsonEntry *e = &j->u.object.entries[order ? order[k] : k];
    separate(w, written++);
    tz_json_quote(&w->out, e->key);
    tz_buf_ascii(&w->out, w->gap.length ? ": " : ":");
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
  for (uint32_t i = 0; i < count; ++i) {
    separate(w, i);
    const void *value = data + (size_t)i * stride;
    if (!(type ? write_value(w, type, value) : write_json(w, *(TzJson *const *)value))) {
      tz_buf_ascii(&w->out, "null");
    }
  }
}

static void quote_utf8(TzBuf *out, const char *name, size_t n) {
  TzBuf key;
  tz_buf_init(&key);
  for (size_t i = 0; i < n;) {
    uint32_t c = (unsigned char)name[i];
    size_t extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (extra) {
      c &= 0x3Fu >> extra;
    }
    for (size_t k = 1; k <= extra && i + k < n; ++k) {
      c = (c << 6) | ((unsigned char)name[i + k] & 0x3F);
    }
    i += extra + 1;
    if (c >= 0x10000) {
      c -= 0x10000;
      tz_buf_push(&key, (uint16_t)(0xD800 + (c >> 10)));
      tz_buf_push(&key, (uint16_t)(0xDC00 + (c & 0x3FF)));
    } else {
      tz_buf_push(&key, (uint16_t)c);
    }
  }
  TzStr s = tz_buf_take(&key);
  tz_json_quote(out, s);
  tz_str_release_value(s);
}

static int write_value(Writer *w, const TzType *type, const void *value) {
  TzBuf *out = &w->out;
  if (!type) {
    const TzArray *a = *(TzArray *const *)value;
    enter(w, a);
    tz_buf_push(out, '[');
    write_elements(w, NULL, a->data, a->length, sizeof(TzJson *));
    leave(w);
    close_with(w, a->length, ']');
    return 1;
  }
  type = tz_resolve(type, &value);
  switch (type->kind) {
  case TZ_T_INT8:
  case TZ_T_INT16:
  case TZ_T_INT32:
  case TZ_T_INT64:
  case TZ_T_UINT8:
  case TZ_T_UINT16:
  case TZ_T_UINT32:
  case TZ_T_UINT64:
    tz_buf_integer(out, type->kind, tz_load_integer(type->kind, value));
    return 1;
  case TZ_T_DOUBLE:
    write_number(out, *(const double *)value);
    return 1;
  case TZ_T_BOOL:
    tz_buf_ascii(out, *(const uint8_t *)value ? "true" : "false");
    return 1;
  case TZ_T_STRING:
    tz_json_quote(out, *(const TzStr *)value);
    return 1;
  case TZ_T_DECIMAL: {
    TzBuf text;
    tz_buf_init(&text);
    tz_buf_decimal(&text, *(const TzDec *)value);
    TzStr s = tz_buf_take(&text);
    tz_json_quote(out, s);
    tz_str_release_value(s);
    return 1;
  }
  case TZ_T_JSON:
    return write_json(w, *(TzJson *const *)value);
  case TZ_T_UNKNOWN: {
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
  case TZ_T_OPTIONAL:
    return tz_optional_has(type, value) && write_value(w, type->inner, value);
  case TZ_T_ARRAY: {
    const TzArray *a = *(TzArray *const *)value;
    enter(w, a);
    tz_buf_push(out, '[');
    write_elements(w, a->type, a->data, a->length, a->type->size);
    leave(w);
    close_with(w, a->length, ']');
    return 1;
  }
  case TZ_T_RESULT: {
    const TzResultType *result = (const TzResultType *)type;
    const char *object = (const char *)value;
    int ok = object[result->ok_offset] != 0;
    const TzType *half = ok ? type->inner : result->error;
    const void *held = object + (ok ? result->value_offset : result->error_offset);
    enter(w, value);
    tz_buf_push(out, '{');
    uint32_t written = 0;
    if (has_json(half, held)) {
      separate(w, written++);
      tz_buf_ascii(out, ok ? "\"value\"" : "\"error\"");
      tz_buf_ascii(out, w->gap.length ? ": " : ":");
      write_value(w, half, held);
    }
    leave(w);
    close_with(w, written, '}');
    return 1;
  }
  case TZ_T_OBJECT: {
    const TzInstance *object = *(TzInstance *const *)value;
    const TzLayout *layout = object->layout;
    const char *fields = (const char *)object;
    if (layout->flags & TZ_LAYOUT_BOX) {
      return write_value(w, ((const TzBox *)object)->type, TZ_BOX_VALUE(object));
    }
    if (layout->flags & TZ_LAYOUT_CLASS) {
      return 0;
    }
    if (layout->flags & TZ_LAYOUT_MATCH) {
      return write_value(w, layout->fields[0].type, fields + layout->fields[0].offset);
    }
    if (layout->flags & TZ_LAYOUT_DATE) {
      TzStr text;
      tz_date_iso(&text, ((const TzDate *)object)->time);
      tz_json_quote(out, text);
      tz_str_release_value(text);
      return 1;
    }
    if ((layout->flags & TZ_LAYOUT_BYTES) && tz_bytes_kind(object) <= TZ_BYTES_BUFFER) {
      const TzBytes *bytes = (const TzBytes *)object;
      const char *colon = w->gap.length ? ": " : ":";
      enter(w, object);
      tz_buf_push(out, '{');
      int buffer = tz_bytes_kind(object) == TZ_BYTES_BUFFER;
      if (buffer) {
        separate(w, 0);
        tz_buf_ascii(out, "\"type\"");
        tz_buf_ascii(out, colon);
        tz_buf_ascii(out, "\"Buffer\"");
        separate(w, 1);
        tz_buf_ascii(out, "\"data\"");
        tz_buf_ascii(out, colon);
        enter(w, &bytes->data);
        tz_buf_push(out, '[');
      }
      for (int32_t i = 0; i < bytes->length; ++i) {
        separate(w, (uint32_t)i);
        if (!buffer) {
          char key[16];
          int n = snprintf(key, sizeof key, "\"%d\"", i);
          tz_buf_ascii_n(out, key, (size_t)n);
          tz_buf_ascii(out, colon);
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
    enter(w, object);
    if (layout->flags & TZ_LAYOUT_DICT) {
      const TzMap *m = (const TzMap *)object;
      const TzMapLayout *l = m->layout;
      tz_buf_push(out, '{');
      uint32_t written = 0, at = 0;
      if (l->kind == TZ_MAP_RECORD) {
        for (const char *e; (e = (const char *)tz_map_next(m, &at));) {
          if (!has_json(l->value, e + l->value_offset)) {
            continue;
          }
          separate(w, written++);
          tz_json_quote(out, *(const TzStr *)(e + l->key_offset));
          tz_buf_ascii(out, w->gap.length ? ": " : ":");
          write_value(w, l->value, e + l->value_offset);
        }
      }
      leave(w);
      close_with(w, written, '}');
      return 1;
    }
    if (layout->flags & TZ_LAYOUT_TUPLE) {
      tz_buf_push(out, '[');
      for (uint32_t i = 0; i < layout->count; ++i) {
        separate(w, i);
        if (!write_value(w, layout->fields[i].type, fields + layout->fields[i].offset)) {
          tz_buf_ascii(out, "null");
        }
      }
      leave(w);
      close_with(w, layout->count, ']');
      return 1;
    }
    const TzField *name = NULL;
    tz_buf_push(out, '{');
    uint32_t written = 0;
    for (uint32_t i = 0; i < layout->count; ++i) {
      const TzField *field = &layout->fields[i];
      if (field->offset == TZ_FIELD_METHOD || field->offset == TZ_FIELD_GETTER ||
          tz_field_is_symbol(layout, field) || !has_json(field->type, fields + field->offset)) {
        continue;
      }
      separate(w, written++);
      quote_utf8(out, field->name, field->name_length);
      tz_buf_ascii(out, w->gap.length ? ": " : ":");
      write_value(w, field->type, fields + field->offset);
    }
    if (name) {
      separate(w, written++);
      quote_utf8(out, name->name, name->name_length);
      tz_buf_ascii(out, w->gap.length ? ": " : ":");
      write_value(w, name->type, fields + name->offset);
    }
    leave(w);
    close_with(w, written, '}');
    return 1;
  }
  default:
    return 0;
  }
}

int32_t tz_json_stringify(TzStr *out, const TzType *type, const void *value, int32_t space_kind,
                          double space_number, void *space_obj, uint64_t space_meta) {
  Writer w;
  memset(&w, 0, sizeof w);
  *out = tz_str_from_ascii("", 0);
  if (__builtin_setjmp(w.fail)) {
    return 0;
  }
  tz_buf_init(&w.out);
  tz_buf_init(&w.gap);
  tz_buf_init(&w.indent);
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
    *out = tz_str_from_utf16(w.out.data, w.out.length);
  }
  writer_release(&w);

  return written;
}
