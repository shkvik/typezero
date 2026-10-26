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

int64_t tz_arr_live_objects(void);
TzArray *tz_arr_new(const TzType *type, int64_t length, int64_t capacity);
TzArray *tz_arr_new_group(const TzType *type, int64_t length, int64_t capacity);
TzArray *tz_arr_new_length_group(const TzType *type, int64_t length);
void tz_arr_drop_member(TzArray *array);
void tz_arr_free(TzArray *array);
void tz_arr_push_value(TzArray *a, const void *value);
void tz_arr_grow(TzArray *a, int64_t min);
void tz_value_retain(const TzType *type, void *value);
void tz_value_release(const TzType *type, void *value);
int tz_is_managed(const TzType *type);
void tz_buf_value(TzBuf *out, const TzType *type, const void *value, int in_join);
void tz_buf_join(TzBuf *out, const TzArray *array, const TzBuf *sep);

#endif
