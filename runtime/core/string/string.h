// Strings, and the buffers that build them.

#ifndef TZ_STRING_H
#define TZ_STRING_H

#include "runtime/core/tzrt/tzrt.h"

typedef struct {
  void *obj;
  uint64_t meta;
} TzStr;

// A string literal type, or a union of them (TZ_T_ATOM): the value is the
// number of the literal, an int32, and `atoms` the strings by number, the
// same table for all of them (index 0, undefined, is "undefined").
typedef struct TzAtomType {
  TzType base;
  const TzStr *atoms;
} TzAtomType;

extern const TzType tz_string_type, tz_undefined_type;

// What a value of a union or of a literal type holds, for the code that
// takes values of any type: the type and the address of the member it holds,
// or of the string of the literal. Any other value is itself.
static inline const TzType *tz_resolve(const TzType *type, const void **value) {
  if (type->kind == TZ_T_ATOM) {
    *value = &((const TzAtomType *)type)->atoms[*(const int32_t *)*value];
    return &tz_string_type;
  }
  if (type->kind == TZ_T_UNION) {
    uint32_t tag = *(const uint32_t *)*value;
    *value = (const char *)*value + 8;
    type = tag ? ((const TzUnionType *)type)->slots[tag] : &tz_undefined_type;
    // A string literal of the union: its string, as above.
    if (type->kind == TZ_T_ATOM) {
      *value = &((const TzAtomType *)type)->atoms[*(const int32_t *)*value];
      return &tz_string_type;
    }
  }

  return type;
}

int64_t tz_str_live_objects(void);
void tz_str_retain_value(TzStr s);
void tz_str_release_value(TzStr s);
// A new string from UTF-16 code units, stored one byte per unit if possible.
TzStr tz_str_from_utf16(const uint16_t *units, size_t length);
TzStr tz_str_from_ascii(const char *text, size_t length);
// A new string of `length` ASCII characters for the caller to write at the
// pointer returned; NULL for a short one, which tz_str_from_ascii makes.
char *tz_str_ascii_units(size_t length, TzStr *out);
int32_t tz_str_equal(void *a_obj, uint64_t a_meta, void *b_obj, uint64_t b_meta);
int32_t tz_str_compare(void *a_obj, uint64_t a_meta, void *b_obj, uint64_t b_meta);
// `items` joined by `sep`, or by "," when sep is NULL.
void tz_str_join(TzStr *out, const TzStr *items, uint32_t n, const TzStr *sep);
// s[from..to), owned; s is borrowed.
TzStr tz_str_sub(TzStr s, uint32_t from, uint32_t to);
// The code units of `*s`, which becomes its flat form (borrowed, as `*s`
// was): one byte each, or two with `*two`. A short string keeps them in
// `*s` itself, so `*s` must stay where it is while they are read.
const void *tz_str_units(TzStr *s, uint32_t *length, int *two);
// Writes UTF-16 to stdout as UTF-8; lone surrogates become U+FFFD.
void tz_out_write_utf16(const uint16_t *units, size_t length);
// The same for a string.
void tz_print_string(void *obj, uint64_t meta);
void tz_str_from_f64(TzStr *out, double value);

// A growable buffer of UTF-16 code units, for building strings in C.
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
// The same for one-byte code units.
void tz_buf_bytes(TzBuf *b, const uint8_t *units, size_t length);
void tz_buf_str(TzBuf *b, TzStr s);
// Moves the contents into a new string and empties the buffer.
TzStr tz_buf_take(TzBuf *b);

void tz_str_to_lower(TzStr *out, void *obj, uint64_t meta);

// `string | undefined` as the compiler lays it out.
typedef struct {
  TzStr value;
  uint8_t has;
} TzOptStr;

#endif
