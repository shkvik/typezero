#include "runtime/core/array/array.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/closure/closure.h"
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

int64_t tz_arr_live_objects(void) { return tz_thread_sum(offsetof(TzThread, live_arrays)); }

const TzType tz_string_type = {TZ_T_STRING, sizeof(TzStr), NULL};
const TzType tz_undefined_type = {TZ_T_UNDEFINED, 0, NULL};

int tz_is_managed(const TzType *type) {
  switch (type->kind) {
  case TZ_T_UNION:
    for (int slot = 1; slot < TZ_UNION_SLOTS; ++slot) {
      const TzType *part = ((const TzUnionType *)type)->slots[slot];
      if (part && tz_is_managed(part)) {
        return 1;
      }
    }
    return 0;
  case TZ_T_STRING:
  case TZ_T_DECIMAL:
  case TZ_T_ARRAY:
  case TZ_T_FUNCTION:
  case TZ_T_OBJECT:
  case TZ_T_JSON:
  case TZ_T_UNKNOWN:
    return 1;
  case TZ_T_OPTIONAL:
    return tz_is_managed(type->inner);
  case TZ_T_RESULT:
    return tz_is_managed(type->inner) || tz_is_managed(((const TzResultType *)type)->error);
  default:
    return 0;
  }
}

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

static void release_value(const TzType *type, void *value, DeadArrays *dead, void *owner) {
  if (type->kind == TZ_T_UNION) {
    const void *held = value;
    type = tz_resolve(type, &held);
    value = (void *)held;
  }
  switch (type->kind) {
  case TZ_T_STRING:
    tz_str_release_value(*(TzStr *)value);
    break;
  case TZ_T_ARRAY: {
    TzArray *a = *(TzArray **)value;
    if (!a) {
      break;
    }
    if (tz_rc_is_member(a->rc)) {
      if (owner) {
        tz_group_unstore(owner, a);
      } else {
        tz_group_release(a);
      }
    } else if (tz_rc_dec(&a->rc)) {
      dead_push(dead, a);
    }
    break;
  }
  case TZ_T_OPTIONAL:
    release_value(type->inner, value, dead, owner);
    break;
  case TZ_T_FUNCTION: {
    TzObject *env = ((TzFunction *)value)->env;
    if (owner && env && tz_rc_is_member(env->rc)) {
      tz_group_unstore(owner, env);
    } else {
      tz_obj_release(env);
    }
    break;
  }
  case TZ_T_OBJECT:
  case TZ_T_UNKNOWN: {
    TzInstance *o = *(TzInstance **)value;
    if (owner && o && tz_rc_is_member(o->rc)) {
      tz_group_unstore(owner, o);
    } else {
      tz_instance_release(o);
    }
    break;
  }
  case TZ_T_JSON:
    tz_json_release(*(TzJson **)value);
    break;
  case TZ_T_DECIMAL:
    tz_dec_release_value(*(TzDec *)value);
    break;
  case TZ_T_RESULT: {
    const TzResultType *result = (const TzResultType *)type;
    release_value(type->inner, (char *)value + result->value_offset, dead, owner);
    release_value(result->error, (char *)value + result->error_offset, dead, owner);
    break;
  }
  default:
    break;
  }
}

void tz_value_retain(const TzType *type, void *value) {
  if (type->kind == TZ_T_UNION) {
    const void *held = value;
    type = tz_resolve(type, &held);
    value = (void *)held;
  }
  switch (type->kind) {
  case TZ_T_STRING:
    tz_str_retain_value(*(TzStr *)value);
    break;
  case TZ_T_ARRAY: {
    TzArray *a = *(TzArray **)value;
    if (a) {
      if (tz_rc_is_member(a->rc)) {
        tz_group_retain(a);
      } else {
        tz_rc_inc(&a->rc);
      }
    }
    break;
  }
  case TZ_T_OPTIONAL:
    tz_value_retain(type->inner, value);
    break;
  case TZ_T_FUNCTION:
    tz_obj_retain(((TzFunction *)value)->env);
    break;
  case TZ_T_OBJECT:
  case TZ_T_UNKNOWN: {
    TzInstance *o = *(TzInstance **)value;
    if (o) {
      tz_ref_retain(o);
    }
    break;
  }
  case TZ_T_JSON: {
    TzJson *j = *(TzJson **)value;
    if (j) {
      tz_rc_inc(&j->rc);
    }
    break;
  }
  case TZ_T_DECIMAL:
    tz_dec_retain_value(*(TzDec *)value);
    break;
  case TZ_T_RESULT: {
    const TzResultType *result = (const TzResultType *)type;
    tz_value_retain(type->inner, (char *)value + result->value_offset);
    tz_value_retain(result->error, (char *)value + result->error_offset);
    break;
  }
  default:
    break;
  }
}

void tz_arr_free(TzArray *array) {
  DeadArrays dead = {NULL, 0, 0};
  TzArray *a = array;
  for (;;) {
    const TzType *type = a->type;
    if (tz_is_managed(type)) {
      for (uint32_t i = 0; i < a->length; ++i) {
        release_value(type, element(a, i), &dead, NULL);
      }
    }
    if (tz_group_debugging) {
      tz_group_forget_unstored(a);
    }
    tz_free(a->alloc);
    tz_free(a);
    --tz_live_arrays;
    if (dead.count == 0) {
      break;
    }
    a = dead.items[--dead.count];
  }
  tz_free(dead.items);
}

void tz_arr_drop_member(TzArray *a) {
  DeadArrays dead = {NULL, 0, 0};
  if (tz_is_managed(a->type)) {
    for (uint32_t i = 0; i < a->length; ++i) {
      release_value(a->type, element(a, i), &dead, a);
    }
  }
  tz_free(a->alloc);
  a->alloc = NULL;
  a->length = 0;
  while (dead.count > 0) {
    tz_arr_free(dead.items[--dead.count]);
  }
  tz_free(dead.items);
}

void tz_value_release(const TzType *type, void *value) {
  DeadArrays dead = {NULL, 0, 0};
  release_value(type, value, &dead, NULL);
  while (dead.count > 0) {
    tz_arr_free(dead.items[--dead.count]);
  }
  tz_free(dead.items);
}

void tz_value_release_in(void *owner, const TzType *type, void *value) {
  DeadArrays dead = {NULL, 0, 0};
  release_value(type, value, &dead, tz_rc_is_member(((TzObject *)owner)->rc) ? owner : NULL);
  while (dead.count > 0) {
    tz_arr_free(dead.items[--dead.count]);
  }
  tz_free(dead.items);
}

static void release_range(TzArray *a, uint32_t from, uint32_t to) {
  if (!tz_is_managed(a->type)) {
    return;
  }
  DeadArrays dead = {NULL, 0, 0};
  void *owner = tz_rc_is_member(a->rc) ? a : NULL;
  for (uint32_t i = from; i < to; ++i) {
    release_value(a->type, element(a, i), &dead, owner);
  }
  while (dead.count > 0) {
    tz_arr_free(dead.items[--dead.count]);
  }
  tz_free(dead.items);
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

static void retain_range_of(const TzType *type, char *values, uint32_t n) {
  if (tz_is_managed(type)) {
    for (uint32_t i = 0; i < n; ++i) {
      tz_value_retain(type, values + (size_t)i * type->size);
    }
  }
}

TzArray *tz_arr_new(const TzType *type, int64_t length, int64_t capacity) {
  if (capacity < length) {
    capacity = length;
  }
  if (length < 0 || capacity > MAX_LENGTH) {
    invalid_length(length < 0 ? length : capacity);
  }
  TzArray *a = (TzArray *)tz_alloc(sizeof(TzArray) + (size_t)capacity * type->size);
  a->rc = 1;
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

static TzArray *new_like(const TzArray *a, int64_t length) {
  return tz_rc_is_member(a->rc) ? tz_arr_new_group(a->type, length, length)
                                : tz_arr_new(a->type, length, length);
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

TZ_COLD void tz_arr_grow(TzArray *a, int64_t min) {
  if (min > MAX_LENGTH) {
    invalid_length(min);
  }
  uint64_t capacity = (uint64_t)a->length + a->length / 2 + 8;
  if (capacity < (uint64_t)min) {
    capacity = (uint64_t)min;
  }
  if (capacity > MAX_LENGTH) {
    capacity = MAX_LENGTH;
  }
  reallocate(a, 0, (uint32_t)capacity);
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

void *tz_arr_unshift_slots(TzArray *a, int64_t n) {
  if (n == 0) {
    return a->data;
  }
  size_t size = a->type->size;
  uint64_t length = (uint64_t)a->length + (uint64_t)n;
  if (length > MAX_LENGTH) {
    invalid_length((int64_t)length);
  }
  uint64_t front = (uint64_t)(a->data - storage(a)) / size;
  if (front >= (uint64_t)n) {
    a->data -= (size_t)n * size;
    a->capacity += (uint32_t)n;
    a->length = (uint32_t)length;
    return a->data;
  }
  uint64_t spare = length / 2 + 8;
  uint64_t newFront = spare / 2;
  uint64_t capacity = length + (spare - newFront);
  if (capacity > MAX_LENGTH) {
    capacity = length;
  }
  char *buffer = (char *)tz_alloc((size_t)(newFront + capacity) * size);
  char *data = buffer + (size_t)newFront * size;
  if (a->length) {
    memcpy(data + (size_t)n * size, a->data, (size_t)a->length * size);
  }
  tz_free(a->alloc);
  a->alloc = buffer;
  a->data = data;
  a->capacity = (uint32_t)capacity;
  a->length = (uint32_t)length;

  return data;
}

void tz_arr_set_length(TzArray *a, int64_t n) {
  if (n < 0 || n > MAX_LENGTH) {
    invalid_length(n);
  }
  size_t size = a->type->size;
  uint32_t old = a->length;
  uint32_t kind = a->type->kind;
  if ((uint64_t)n > old &&
      (kind == TZ_T_ARRAY || kind == TZ_T_FUNCTION || kind == TZ_T_OBJECT || kind == TZ_T_JSON)) {
    tz_panicf("internal error: an array of %s grows by setting its length",
              kind == TZ_T_ARRAY      ? "arrays"
              : kind == TZ_T_FUNCTION ? "functions"
              : kind == TZ_T_JSON     ? "Json values"
                                      : "objects");
  }
  if ((uint64_t)n < old) {
    a->length = (uint32_t)n;
    release_range(a, (uint32_t)n, old);
  } else if ((uint64_t)n > old) {
    if ((uint64_t)n > a->capacity) {
      tz_arr_grow(a, n);
    }
    memset(element(a, old), 0, ((size_t)n - old) * size);
    a->length = (uint32_t)n;
  }
}

static int64_t relative(int64_t position, int64_t length) {
  if (position < 0) {
    position += length;
    return position < 0 ? 0 : position;
  }

  return position > length ? length : position;
}

TzArray *tz_arr_slice(TzArray *a, int64_t start, int64_t end, int32_t has_end) {
  int64_t length = a->length;
  int64_t from = relative(start, length);
  int64_t to = has_end ? relative(end, length) : length;
  int64_t count = to > from ? to - from : 0;
  TzArray *result = new_like(a, count);
  if (count) {
    memcpy(result->data, element(a, (uint32_t)from), (size_t)count * a->type->size);
    retain_range(result, 0, (uint32_t)count);
  }

  return result;
}

TzArray *tz_arr_copy(TzArray *a) { return tz_arr_slice(a, 0, 0, 0); }

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

TzArray *tz_arr_splice(TzArray *a, int64_t start, int64_t delete_count, int32_t has_delete,
                       int64_t insert, void **slots) {
  int64_t length = a->length;
  int64_t from = relative(start, length);
  int64_t count = length - from;
  if (has_delete && delete_count < count) {
    count = delete_count < 0 ? 0 : delete_count;
  }
  size_t size = a->type->size;
  TzArray *removed = tz_arr_new(a->type, count, count);
  if (count) {
    memcpy(removed->data, element(a, (uint32_t)from), (size_t)count * size);
    if (tz_rc_is_member(a->rc)) {
      for (int64_t i = 0; i < count; ++i) {
        tz_group_take_value(a, a->type, removed->data + (size_t)i * size);
      }
    }
  }
  int64_t new_length = length - count + insert;
  if (new_length > MAX_LENGTH) {
    invalid_length(new_length);
  }
  if (insert != count) {
    if (new_length > a->capacity) {
      tz_arr_grow(a, new_length);
    }
    memmove(element(a, (uint32_t)(from + insert)), element(a, (uint32_t)(from + count)),
            (size_t)(length - from - count) * size);
  }
  a->length = (uint32_t)new_length;
  *slots = element(a, (uint32_t)from);

  return removed;
}

void tz_arr_append(TzArray *dst, TzArray *src) {
  uint32_t n = src->length;
  if (n == 0) {
    return;
  }
  uint64_t total = (uint64_t)dst->length + n;
  if (total > dst->capacity) {
    tz_arr_grow(dst, (int64_t)total);
  }
  memcpy(element(dst, dst->length), src->data, (size_t)n * dst->type->size);
  uint32_t from = dst->length;
  dst->length = (uint32_t)total;
  retain_range(dst, from, dst->length);
}

static int values_equal(const TzType *type, const void *a, const void *b, int same_value_zero) {
  switch (type->kind) {
  case TZ_T_ATOM:
    return *(const int32_t *)a == *(const int32_t *)b;
  case TZ_T_UNION: {
    uint32_t tag = *(const uint32_t *)a;
    if (tag != *(const uint32_t *)b) {
      return 0;
    }
    if (tag == 0) {
      return 1;
    }
    const void *x = a, *y = b;
    tz_resolve(type, &y);
    type = tz_resolve(type, &x);
    return values_equal(type, x, y, same_value_zero);
  }
  case TZ_T_INT8:
  case TZ_T_INT16:
  case TZ_T_INT32:
  case TZ_T_INT64:
  case TZ_T_UINT8:
  case TZ_T_UINT16:
  case TZ_T_UINT32:
  case TZ_T_UINT64:
    return tz_load_integer(type->kind, a) == tz_load_integer(type->kind, b);
  case TZ_T_DOUBLE: {
    double x = *(const double *)a, y = *(const double *)b;
    return x == y || (same_value_zero && isnan(x) && isnan(y));
  }
  case TZ_T_BOOL:
    return (*(const uint8_t *)a != 0) == (*(const uint8_t *)b != 0);
  case TZ_T_STRING: {
    const TzStr *x = (const TzStr *)a, *y = (const TzStr *)b;
    return tz_str_equal(x->obj, x->meta, y->obj, y->meta);
  }
  case TZ_T_ARRAY:
  case TZ_T_OBJECT:
    return *(void *const *)a == *(void *const *)b;
  case TZ_T_JSON:
    return tz_json_strict_equal(*(TzJson *const *)a, *(TzJson *const *)b);
  case TZ_T_UNKNOWN:
    return tz_unknown_equal(*(void *const *)a, *(void *const *)b);
  case TZ_T_DECIMAL: {
    const TzDec *x = (const TzDec *)a, *y = (const TzDec *)b;
    return tz_dec_equal(x->coef, x->meta, y->coef, y->meta);
  }
  case TZ_T_FUNCTION: {
    const TzFunction *x = (const TzFunction *)a, *y = (const TzFunction *)b;
    return x->info == y->info && x->env == y->env;
  }
  case TZ_T_OPTIONAL: {
    int has_a = tz_optional_has(type, a), has_b = tz_optional_has(type, b);
    if (!has_a || !has_b) {
      return has_a == has_b;
    }
    return values_equal(type->inner, a, b, same_value_zero);
  }
  default:
    return 0;
  }
}

int32_t tz_arr_index_of(TzArray *a, const void *value, int64_t from, int32_t has_from,
                        int32_t mode) {
  int64_t length = a->length;
  if (mode == 1) {
    int64_t i = !has_from  ? length - 1
                : from < 0 ? length + from
                           : (from < length ? from : length - 1);
    for (; i >= 0; --i) {
      if (values_equal(a->type, element(a, (uint32_t)i), value, 0)) {
        return (int32_t)i;
      }
    }
    return -1;
  }
  for (int64_t i = has_from ? relative(from, length) : 0; i < length; ++i) {
    if (values_equal(a->type, element(a, (uint32_t)i), value, mode == 2)) {
      return mode == 2 ? 1 : (int32_t)i;
    }
  }

  return mode == 2 ? 0 : -1;
}

void tz_buf_value(TzBuf *out, const TzType *type, const void *value, int in_join) {
  char text[64];
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
    break;
  case TZ_T_DOUBLE:
    tz_buf_ascii_n(out, text, tz_format_f64(*(const double *)value, text));
    break;
  case TZ_T_BOOL:
    tz_buf_ascii(out, *(const uint8_t *)value ? "true" : "false");
    break;
  case TZ_T_STRING:
    tz_buf_str(out, *(const TzStr *)value);
    break;
  case TZ_T_DECIMAL:
    tz_buf_decimal(out, *(const TzDec *)value);
    break;
  case TZ_T_ARRAY:
    tz_buf_join(out, *(TzArray *const *)value, NULL);
    break;
  case TZ_T_OBJECT: {
    const TzInstance *object = *(const TzInstance *const *)value;
    if (object && (object->layout->flags & TZ_LAYOUT_REGEX)) {
      tz_buf_regex(out, (const TzRegex *)object);
    } else if (object && (object->layout->flags & TZ_LAYOUT_DATE)) {
      TzStr text;
      tz_date_local_string(&text, ((const TzDate *)object)->time, 0);
      tz_buf_str(out, text);
      tz_str_release_value(text);
    }
    break;
  }
  case TZ_T_OPTIONAL:
    if (tz_optional_has(type, value)) {
      tz_buf_value(out, type->inner, value, in_join);
      break;
    }
  case TZ_T_UNDEFINED:
    if (!in_join) {
      tz_buf_ascii(out, "undefined");
    }
    break;
  default:
    break;
  }
}

void tz_buf_join(TzBuf *out, const TzArray *a, const TzBuf *sep) {
  for (uint32_t i = 0; i < a->length; ++i) {
    if (i > 0) {
      if (sep) {
        tz_buf_units(out, sep->data, sep->length);
      } else {
        tz_buf_push(out, ',');
      }
    }
    tz_buf_value(out, a->type, element(a, i), 1);
  }
}

void tz_arr_join(TzStr *out, TzArray *a, void *sep_obj, uint64_t sep_meta, int32_t has_sep) {
  TzStr separator = {sep_obj, sep_meta};
  if (a->type->kind == TZ_T_STRING) {
    tz_str_join(out, (const TzStr *)a->data, a->length, has_sep ? &separator : NULL);
    return;
  }
  TzBuf buf, sep;
  tz_buf_init(&buf);
  tz_buf_init(&sep);
  if (has_sep) {
    tz_buf_str(&sep, separator);
  }
  tz_buf_join(&buf, a, has_sep ? &sep : NULL);
  tz_buf_free(&sep);
  *out = tz_buf_take(&buf);
}

void tz_value_to_string(TzStr *out, const TzType *type, const void *value) {
  type = tz_resolve(type, &value);
  if (type->kind == TZ_T_STRING) {
    *out = *(const TzStr *)value;
    tz_str_retain_value(*out);
    return;
  }
  if (type->kind == TZ_T_OPTIONAL && tz_optional_has(type, value) &&
      type->inner->kind == TZ_T_STRING) {
    *out = *(const TzStr *)value;
    tz_str_retain_value(*out);
    return;
  }
  TzBuf buf;
  tz_buf_init(&buf);
  tz_buf_value(&buf, type, value, 0);
  *out = tz_buf_take(&buf);
}

void tz_arr_reverse(TzArray *a) {
  size_t size = a->type->size;
  char small[32];
  char *tmp = size <= sizeof small ? small : (char *)tz_alloc(size);
  for (uint32_t i = 0, j = a->length; i + 1 < j; ++i, --j) {
    char *x = element(a, i), *y = element(a, j - 1);
    memcpy(tmp, x, size);
    memcpy(x, y, size);
    memcpy(y, tmp, size);
  }
  if (tmp != small) {
    tz_free(tmp);
  }
}

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

static int sort_before(const SortItem *a, const SortItem *b) {
  if (a->undefined || b->undefined) {
    return !a->undefined && b->undefined;
  }

  return tz_str_compare(a->key.obj, a->key.meta, b->key.obj, b->key.meta) < 0;
}

static void merge_sort(SortItem *items, SortItem *tmp, size_t n) {
  if (n < 2) {
    return;
  }
  size_t half = n / 2;
  merge_sort(items, tmp, half);
  merge_sort(items + half, tmp, n - half);
  size_t i = 0, j = half, k = 0;
  while (i < half && j < n) {
    tmp[k++] = sort_before(&items[j], &items[i]) ? items[j++] : items[i++];
  }
  while (i < half) {
    tmp[k++] = items[i++];
  }
  while (j < n) {
    tmp[k++] = items[j++];
  }
  memcpy(items, tmp, n * sizeof(SortItem));
}

typedef int32_t (*TzCompare)(void *ctx, const void *x, const void *y);

static void merge_sort_with(char *items, char *tmp, size_t n, size_t size, TzCompare compare,
                            void *ctx) {
  if (n < 2) {
    return;
  }
  size_t half = n / 2;
  merge_sort_with(items, tmp, half, size, compare, ctx);
  merge_sort_with(items + half * size, tmp, n - half, size, compare, ctx);
  size_t i = 0, j = half, k = 0;
  while (i < half && j < n) {
    if (compare(ctx, items + i * size, items + j * size) > 0) {
      memcpy(tmp + k++ * size, items + j++ * size, size);
    } else {
      memcpy(tmp + k++ * size, items + i++ * size, size);
    }
  }
  while (i < half) {
    memcpy(tmp + k++ * size, items + i++ * size, size);
  }
  while (j < n) {
    memcpy(tmp + k++ * size, items + j++ * size, size);
  }
  memcpy(items, tmp, n * size);
}

void tz_arr_sort_with(TzArray *a, TzCompare compare, void *ctx) {
  uint32_t n = a->length;
  if (n < 2) {
    return;
  }
  const TzType *type = a->type;
  size_t size = type->size;
  char *items = (char *)tz_alloc((size_t)n * size);
  char *tmp = (char *)tz_alloc((size_t)n * size);
  size_t defined = 0, undefined = n;
  for (uint32_t i = 0; i < n; ++i) {
    const char *value = element(a, i);
    if (type->kind == TZ_T_OPTIONAL && !tz_optional_has(type, value)) {
      memcpy(tmp + (size_t)--undefined * size, value, size);
    } else {
      memcpy(items + defined++ * size, value, size);
    }
  }
  for (size_t i = defined; i < n; ++i) {
    memcpy(items + i * size, tmp + (n - 1 - (i - defined)) * size, size);
  }
  retain_range_of(type, items, n);
  merge_sort_with(items, tmp, defined, size, compare, ctx);
  if (a->length == n) {
    release_range(a, 0, n);
    memcpy(a->data, items, (size_t)n * size);
    if (tz_rc_is_member(a->rc) && tz_is_managed(type)) {
      for (uint32_t i = 0; i < n; ++i) {
        tz_group_store_value(a, type, element(a, i));
      }
    }
  } else if (tz_is_managed(type)) {
    for (uint32_t i = 0; i < n; ++i) {
      tz_value_release(type, items + (size_t)i * size);
    }
  }
  tz_free(items);
  tz_free(tmp);
}

void tz_arr_sort(TzArray *a) {
  uint32_t n = a->length;
  if (n < 2) {
    return;
  }
  const TzType *type = a->type;
  size_t size = type->size;
  SortItem *items = (SortItem *)tz_alloc(n * sizeof(SortItem));
  SortItem *tmp = (SortItem *)tz_alloc(n * sizeof(SortItem));
  for (uint32_t i = 0; i < n; ++i) {
    const char *value = element(a, i);
    const TzType *t = type;
    SortItem *item = &items[i];
    item->index = i;
    item->undefined = 0;
    item->owned = 0;
    if (t->kind == TZ_T_OPTIONAL) {
      if (!tz_optional_has(t, value)) {
        item->undefined = 1;
        item->key.obj = NULL;
        item->key.meta = 0;
        continue;
      }
      t = t->inner;
    }
    if (t->kind == TZ_T_STRING) {
      item->key = *(const TzStr *)value;
    } else {
      TzBuf buf;
      tz_buf_init(&buf);
      tz_buf_value(&buf, t, value, 0);
      item->key = tz_buf_take(&buf);
      item->owned = 1;
    }
  }
  merge_sort(items, tmp, n);

  char *sorted = (char *)tz_alloc((size_t)n * size);
  for (uint32_t k = 0; k < n; ++k) {
    memcpy(sorted + (size_t)k * size, element(a, items[k].index), size);
  }
  memcpy(a->data, sorted, (size_t)n * size);
  tz_free(sorted);
  for (uint32_t i = 0; i < n; ++i) {
    if (items[i].owned) {
      tz_str_release_value(items[i].key);
    }
  }
  tz_free(items);
  tz_free(tmp);
}
