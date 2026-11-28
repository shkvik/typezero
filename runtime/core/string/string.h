#ifndef TZ_STRING_H
#define TZ_STRING_H

#include "runtime/core/tzrt/tzrt.h"

typedef struct {
  void *obj;
  uint64_t meta;
} TzStr;

typedef struct TzAtomType {
  TzType base;
  const TzStr *atoms;
} TzAtomType;

extern const TzType tz_string_type, tz_undefined_type;

static inline const TzType *tz_resolve(const TzType *type, const void **value) {
  if (type->kind == TZ_T_ATOM) {
    *value = &((const TzAtomType *)type)->atoms[*(const int32_t *)*value];
    return &tz_string_type;
  }
  if (type->kind == TZ_T_UNION) {
    uint32_t tag = *(const uint32_t *)*value;
    *value = (const char *)*value + 8;
    type = tag ? ((const TzUnionType *)type)->slots[tag] : &tz_undefined_type;
    if (type->kind == TZ_T_ATOM) {
      *value = &((const TzAtomType *)type)->atoms[*(const int32_t *)*value];
      return &tz_string_type;
    }
  }

  return type;
}

void tz_str_retain_value(TzStr s);
void tz_str_release_value(TzStr s);
TzStr tz_str_from_utf16(const uint16_t *units, size_t length);
TzStr tz_str_from_ascii(const char *text, size_t length);
char *tz_str_ascii_units(size_t length, TzStr *out);
const void *tz_str_units(TzStr *s, uint32_t *length, int *two);

typedef struct {
  uint16_t *data;
  size_t length, capacity;
} TzBuf;

void tz_buf_init(TzBuf *b);
void tz_buf_free(TzBuf *b);
void tz_buf_push(TzBuf *b, uint16_t unit);
void tz_buf_ascii(TzBuf *b, const char *text);
void tz_buf_ascii_n(TzBuf *b, const char *text, size_t length);
void tz_buf_units(TzBuf *b, const uint16_t *units, size_t length);
void tz_buf_bytes(TzBuf *b, const uint8_t *units, size_t length);
void tz_buf_utf8(TzBuf *b, const char *text, size_t length);
void tz_buf_str(TzBuf *b, TzStr s);
TzStr tz_buf_take(TzBuf *b);


typedef struct {
  TzStr value;
  uint8_t has;
} TzOptStr;

#endif
