#include "runtime/core/array/array.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/closure/closure.h"
#include "runtime/core/string/cstring.h"
#include "runtime/core/error/error.h"
#include "runtime/core/group/group.h"
#include "runtime/core/output/output.h"
#include "runtime/core/parallel/parallel.h"
#include "runtime/core/publish/publish.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include "runtime/core/unknown/unknown.h"
#include "runtime/library/date/date/date.h"
#include "runtime/library/decimal/decimal/decimal.h"
#include "runtime/library/json/json/json.h"
#include "runtime/library/regex/regex/regex.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LENGTH ((1u << 30) - 1)

const TzType tz_string_type = {TZ_T_STRING, sizeof(TzStr), NULL};
const TzType tz_undefined_type = {TZ_T_UNDEFINED, 0, NULL};

static inline char *element(const TzArray *a, uint32_t i) {
  return a->data + (size_t)i * a->type->size;
}

static inline char *inline_storage(TzArray *a) { return (char *)(a + 1); }

static inline char *storage(TzArray *a) { return a->alloc ? a->alloc : inline_storage(a); }

TZ_NORETURN static void invalid_length(int64_t length) {
  if (length < 0) {
    tz_panicf("internal error: the array length %lld", (long long)length);
  }
  tz_panicf("array length %lld exceeds the maximum of %u", (long long)length, MAX_LENGTH);
}

typedef struct {
  TzArray **items;
  size_t count, capacity;
} DeadArrays;

static void dead_push(DeadArrays *dead, TzArray *a) {
  if (dead->count == dead->capacity) {
    size_t capacity = dead->capacity * 2 + 8;
    TzArray **items = (TzArray **)tz_alloc(capacity * sizeof(TzArray *));
    if (dead->count) {
      memcpy(items, dead->items, dead->count * sizeof(TzArray *));
    }
    tz_free(dead->items);
    dead->items = items;
    dead->capacity = capacity;
  }
  dead->items[dead->count++] = a;
}

static void retain_range(TzArray *a, uint32_t from, uint32_t to) {
  if (!tz_is_managed(a->type)) {
    return;
  }
  for (uint32_t i = from; i < to; ++i) {
    tz_value_retain(a->type, element(a, i));
  }
  if (tz_rc_is_member(a->rc)) {
    for (uint32_t i = from; i < to; ++i) {
      tz_group_store_value(a, a->type, element(a, i));
    }
  }
#if TZ_PARALLEL
  else if (a->rc < 0) {
    for (uint32_t i = from; i < to; ++i) {
      tz_publish_value(a->type, element(a, i));
    }
  }
#endif
}

TzArray *tz_arr_new_group(const TzType *type, int64_t length, int64_t capacity) {
  if (capacity < length) {
    capacity = length;
  }
  if (length < 0 || capacity > MAX_LENGTH) {
    invalid_length(length < 0 ? length : capacity);
  }
  TzArray *a = (TzArray *)tz_group_alloc_tagged(sizeof(TzArray) + (size_t)capacity * type->size,
                                                TZ_GROUP_ARRAY);
  a->type = type;
  a->data = inline_storage(a);
  a->length = (uint32_t)length;
  a->capacity = (uint32_t)capacity;
  a->alloc = NULL;
  if (length) {
    memset(a->data, 0, (size_t)length * type->size);
  }
  ++tz_live_arrays;

  return a;
}

TzArray *tz_arr_new_length(const TzType *type, int64_t length) {
  if (length < 0 || length > MAX_LENGTH) {
    invalid_length(length);
  }

  return tz_arr_new(type, length, length);
}

TzArray *tz_arr_new_length_group(const TzType *type, int64_t length) {
  if (length < 0 || length > MAX_LENGTH) {
    invalid_length(length);
  }

  return tz_arr_new_group(type, length, length);
}

static void reallocate(TzArray *a, uint32_t front, uint32_t capacity) {
  size_t size = a->type->size;
  char *buffer = (char *)tz_alloc(((size_t)front + capacity) * size);
  char *data = buffer + (size_t)front * size;
  if (a->length) {
    memcpy(data, a->data, (size_t)a->length * size);
  }
  tz_free(a->alloc);
  a->alloc = buffer;
  a->data = data;
  a->capacity = capacity;
}

TZ_HOT int32_t tz_arr_shift(TzArray *a, void *out) {
  size_t size = a->type->size;
  if (a->length == 0) {
    memset(out, 0, size);
    return 0;
  }
  memcpy(out, a->data, size);
  a->data += size;
  --a->length;
  --a->capacity;
  if (a->length == 0) {
    char *base = storage(a);
    a->capacity += (uint32_t)((size_t)(a->data - base) / size);
    a->data = base;
  }

  return 1;
}

void tz_arr_push_value(TzArray *a, const void *value) {
  if (a->length == a->capacity) {
    tz_arr_grow(a, (int64_t)a->length + 1);
  }
  memcpy(element(a, a->length), value, a->type->size);
  if (a->rc < 0) {
    tz_group_store_value(a, a->type, element(a, a->length));
    tz_store_barrier(a, a->type, element(a, a->length));
  }
  ++a->length;
}

static int64_t relative(int64_t position, int64_t length) {
  if (position < 0) {
    position += length;
    return position < 0 ? 0 : position;
  }

  return position > length ? length : position;
}

/* A slice that only looks at the items (Read<T>[]): no counts; the result never drops them. */
static TzArray *slice_as(TzArray *a, int64_t start, int64_t end, int32_t has_end, int member) {
  int64_t length = a->length;
  int64_t from = relative(start, length);
  int64_t to = has_end ? relative(end, length) : length;
  int64_t count = to > from ? to - from : 0;
  TzArray *result =
      member ? tz_arr_new_group(a->type, count, count) : tz_arr_new(a->type, count, count);
  if (count) {
    memcpy(result->data, element(a, (uint32_t)from), (size_t)count * a->type->size);
    retain_range(result, 0, (uint32_t)count);
  }

  return result;
}

TzArray *tz_arr_slice_plain(TzArray *a, int64_t start, int64_t end, int32_t has_end) {
  return slice_as(a, start, end, has_end, 0);
}

TzArray *tz_arr_slice_group(TzArray *a, int64_t start, int64_t end, int32_t has_end) {
  return slice_as(a, start, end, has_end, 1);
}

TzArray *tz_arr_copy_plain(TzArray *a) { return slice_as(a, 0, 0, 0, 0); }

TzArray *tz_arr_copy_group(TzArray *a) { return slice_as(a, 0, 0, 0, 1); }

/* Moves the items of src to the end of dst: no count changes, src is left empty. */
void tz_arr_fill(TzArray *a, const void *value, int64_t start, int64_t end, int32_t has_start,
                 int32_t has_end) {
  int64_t length = a->length;
  int64_t from = has_start ? relative(start, length) : 0;
  int64_t to = has_end ? relative(end, length) : length;
  size_t size = a->type->size;
  int managed = tz_is_managed(a->type);
  for (int64_t i = from; i < to; ++i) {
    char *slot = element(a, (uint32_t)i);
    if (managed) {
      tz_value_retain(a->type, (void *)value);
      tz_value_release_in(a, a->type, slot);
    }
    memcpy(slot, value, size);
    if (managed && a->rc < 0) {
      tz_group_store_value(a, a->type, slot);
      tz_store_barrier(a, a->type, slot);
    }
  }
}

typedef struct {
  TzStr key;
  uint32_t index;
  uint8_t undefined;
  uint8_t owned;
} SortItem;

typedef int32_t (*TzCompare)(void *ctx, const void *x, const void *y);

