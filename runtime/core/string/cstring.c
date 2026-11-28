#include "runtime/core/string/cstring.h"
#include "runtime/core/closure/closure.h"
#include <string.h>

static void cstring_drop(void *object) {
  tz_obj_free(object);
}

const TzLayout tz_cstring_layout = {cstring_drop, NULL, 0, TZ_LAYOUT_CSTRING, "cstring", 7, 0};

static TzCString *cstring_new(size_t length) {
  TzCString *made = (TzCString *)tz_obj_alloc((int64_t)(sizeof(TzCString) + length + 1));
  made->rc = 1;
  made->layout = &tz_cstring_layout;
  made->length = (int64_t)length;
  made->text[length] = 0;
  return made;
}

static size_t utf8_length(uint32_t code) {
  return code < 0x80 ? 1 : code < 0x800 ? 2 : code < 0x10000 ? 3 : 4;
}

static char *utf8_put(char *at, uint32_t code) {
  if (code < 0x80) {
    *at++ = (char)code;
  } else if (code < 0x800) {
    *at++ = (char)(0xC0 | (code >> 6));
    *at++ = (char)(0x80 | (code & 0x3F));
  } else if (code < 0x10000) {
    *at++ = (char)(0xE0 | (code >> 12));
    *at++ = (char)(0x80 | ((code >> 6) & 0x3F));
    *at++ = (char)(0x80 | (code & 0x3F));
  } else {
    *at++ = (char)(0xF0 | (code >> 18));
    *at++ = (char)(0x80 | ((code >> 12) & 0x3F));
    *at++ = (char)(0x80 | ((code >> 6) & 0x3F));
    *at++ = (char)(0x80 | (code & 0x3F));
  }
  return at;
}

static uint32_t code_at(const void *units, int two, uint32_t length, uint32_t *i) {
  if (!two) {
    return ((const uint8_t *)units)[(*i)++];
  }
  const uint16_t *wide = (const uint16_t *)units;
  uint32_t unit = wide[(*i)++];
  if (unit >= 0xD800 && unit <= 0xDBFF && *i < length && wide[*i] >= 0xDC00 && wide[*i] <= 0xDFFF) {
    uint32_t low = wide[(*i)++];
    return 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
  }
  if (unit >= 0xD800 && unit <= 0xDFFF) {
    return 0xFFFD;
  }
  return unit;
}

void *tz_cstring_from_str(void *obj, uint64_t meta) {
  TzStr s = {obj, meta};
  uint32_t length = 0;
  int two = 0;
  const void *units = tz_str_units(&s, &length, &two);
  size_t bytes = 0;
  for (uint32_t i = 0; i < length;) {
    uint32_t code = code_at(units, two, length, &i);
    if (code == 0) {
      return NULL;
    }
    bytes += utf8_length(code);
  }
  TzCString *made = cstring_new(bytes);
  char *at = made->text;
  for (uint32_t i = 0; i < length;) {
    at = utf8_put(at, code_at(units, two, length, &i));
  }
  return made;
}

void *tz_cstring_copy(const char *text) {
  size_t length = strlen(text);
  TzCString *made = cstring_new(length);
  memcpy(made->text, text, length);
  return made;
}

void tz_cstring_to_str(TzStr *out, const void *cstring) {
  const TzCString *c = (const TzCString *)cstring;
  TzBuf buf;
  tz_buf_init(&buf);
  tz_buf_utf8(&buf, c->text, (size_t)c->length);
  *out = tz_buf_take(&buf);
}

int32_t tz_cstring_equal(const void *a, const void *b) {
  const TzCString *x = (const TzCString *)a;
  const TzCString *y = (const TzCString *)b;
  if (x == y) {
    return 1;
  }
  if (!x || !y || x->length != y->length) {
    return 0;
  }
  return memcmp(x->text, y->text, (size_t)x->length) == 0;
}

