#ifndef TZ_ARRAY_H
#define TZ_ARRAY_H

#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"

typedef struct TzArray {
  int64_t rc;
  const TzType *type;
  char *data;
  uint32_t length;
  uint32_t capacity;
  char *alloc;
} TzArray;

/* An array made immortal (Interned, a copyable module constant) is never freed and is no leak. */
TzArray *tz_arr_new_group(const TzType *type, int64_t length, int64_t capacity);
TzArray *tz_arr_new_length_group(const TzType *type, int64_t length);
void tz_arr_push_value(TzArray *a, const void *value);

#endif
