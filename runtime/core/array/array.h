// Arrays.

#ifndef TZ_ARRAY_H
#define TZ_ARRAY_H

#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"

// An array: a reference-counted header with the elements stored unboxed,
// either right after the header or in a separate buffer once the array grew.
// `data` may point past the start of the storage after shift().
typedef struct TzArray {
  int64_t rc;
  const TzType *type; // of the elements
  char *data;         // the first element
  uint32_t length;
  uint32_t capacity; // elements that fit from `data` on
  char *alloc;       // the separate buffer, or NULL for inline elements
} TzArray;

int64_t tz_arr_live_objects(void);
TzArray *tz_arr_new(const TzType *type, int64_t length, int64_t capacity);
// The same as a member of a group of its own (group.c), for an array of a
// type that may form cycles.
TzArray *tz_arr_new_group(const TzType *type, int64_t length, int64_t capacity);
TzArray *tz_arr_new_length_group(const TzType *type, int64_t length);
// What the group of a member array frees first: its elements (those of the
// group stay) and its buffer, not the header.
void tz_arr_drop_member(TzArray *array);
// Frees an array whose count reached zero.
void tz_arr_free(TzArray *array);
// Appends the value at `value`, which the array takes over.
void tz_arr_push_value(TzArray *a, const void *value);
// Room for at least `min` elements.
void tz_arr_grow(TzArray *a, int64_t min);
void tz_value_retain(const TzType *type, void *value);
void tz_value_release(const TzType *type, void *value);
int tz_is_managed(const TzType *type);
// JavaScript's String(value), appended to `out`. Inside join(), undefined
// elements become empty instead.
void tz_buf_value(TzBuf *out, const TzType *type, const void *value, int in_join);
// Appends the elements of `array` joined by `sep` (NULL for ",").
void tz_buf_join(TzBuf *out, const TzArray *array, const TzBuf *sep);

#endif
