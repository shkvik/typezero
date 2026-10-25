// Arrays: JavaScript semantics (reference objects, live length, the methods
// below behave as in JavaScript) with elements stored unboxed by their static
// type, so an int[] is a plain buffer of 32-bit integers.
//
// The header and its fields are described in tzrt.h (TzArray); the compiler
// reads and writes `data`, `length` and `capacity` directly for indexing,
// push and pop, and calls these functions for everything else.
//
// Storage: a new array keeps its elements right after the header (one
// allocation). When it outgrows them, they move to a separate buffer with
// spare room. shift() advances `data` instead of moving the elements, and
// unshift() leaves room in front, so queues and deques are O(1) per operation.
//
// An array owns its elements: the ones that are strings, arrays or optionals
// of those are reference counted, and released when the array dies.

#include "tzrt.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// The same limit as for strings, which keeps every length far from the int
// range (see kMaxLength in src/sema/facts.h).
#define MAX_LENGTH ((1u << 30) - 1)

int64_t tz_live_arrays; // not static: see the rules in tzrt.h

int64_t tz_arr_live_objects(void) { return tz_live_arrays; }

const TzType tz_string_type = {TZ_T_STRING, sizeof(TzStr), NULL};
const TzType tz_undefined_type = {TZ_T_UNDEFINED, 0, NULL};

int tz_is_managed(const TzType *type) {
  switch (type->kind) {
  case TZ_T_UNION:
    for (int slot = 1; slot < TZ_UNION_SLOTS; ++slot) {
      const TzType *part = ((const TzUnionType *)type)->slots[slot];
      if (part && tz_is_managed(part))
        return 1;
    }
    return 0;
  case TZ_T_STRING:
  case TZ_T_DECIMAL: // its long form
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

// What JavaScript reports as a RangeError.
TZ_NORETURN static void invalid_length(int64_t length) {
  if (length < 0)
    tz_panicf("RangeError: Invalid array length %lld", (long long)length);
  tz_panicf("array length %lld exceeds the maximum of %u", (long long)length, MAX_LENGTH);
}

//===----------------------------------------------------------------------===//
// Values and reference counts
//===----------------------------------------------------------------------===//

// Arrays whose count reached zero, freed one by one so that deeply nested
// arrays do not recurse.
typedef struct {
  TzArray **items;
  size_t count, capacity;
} DeadArrays;

static void dead_push(DeadArrays *dead, TzArray *a) {
  if (dead->count == dead->capacity) {
    size_t capacity = dead->capacity * 2 + 8;
    TzArray **items = (TzArray **)tz_alloc(capacity * sizeof(TzArray *));
    if (dead->count)
      memcpy(items, dead->items, dead->count * sizeof(TzArray *));
    tz_free(dead->items);
    dead->items = items;
    dead->capacity = capacity;
  }
  dead->items[dead->count++] = a;
}

// `owner`, if not NULL, is the object that held the value: a reference it
// held to a member of its own group (or of an eternal one) was not counted
// (see group.c).
static void release_value(const TzType *type, void *value, DeadArrays *dead, void *owner) {
  if (type->kind == TZ_T_UNION) { // the member it holds
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
    if (!a)
      break;
    if (a->rc < 0) {
      if (owner)
        tz_group_unstore(owner, a);
      else
        tz_group_release(a);
    } else if (--a->rc == 0) {
      dead_push(dead, a);
    }
    break;
  }
  case TZ_T_OPTIONAL:
    // Undefined holds the zero value, which releases as nothing.
    release_value(type->inner, value, dead, owner);
    break;
  case TZ_T_FUNCTION: {
    TzObject *env = ((TzFunction *)value)->env;
    if (owner && env && env->rc < 0)
      tz_group_unstore(owner, env);
    else
      tz_obj_release(env);
    break;
  }
  case TZ_T_OBJECT:
  case TZ_T_UNKNOWN: { // a box is an object too
    TzInstance *o = *(TzInstance **)value;
    if (owner && o && o->rc < 0)
      tz_group_unstore(owner, o);
    else
      tz_instance_release(o);
    break;
  }
  case TZ_T_JSON:
    tz_json_release(*(TzJson **)value);
    break;
  case TZ_T_DECIMAL:
    tz_dec_release_value(*(TzDec *)value);
    break;
  case TZ_T_RESULT: {
    // The half not in use holds zero, which releases as nothing.
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
  if (type->kind == TZ_T_UNION) { // the member it holds
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
    if (a)
      tz_ref_retain(a);
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
    if (o)
      tz_ref_retain(o);
    break;
  }
  case TZ_T_JSON: {
    TzJson *j = *(TzJson **)value;
    if (j)
      ++j->rc;
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

// Frees an array whose count reached zero, and everything that dies with it.
// Called by the inline release code that the compiler emits.
void tz_arr_free(TzArray *array) {
  DeadArrays dead = {NULL, 0, 0};
  TzArray *a = array;
  for (;;) {
    const TzType *type = a->type;
    if (tz_is_managed(type))
      for (uint32_t i = 0; i < a->length; ++i)
        release_value(type, element(a, i), &dead, NULL);
    if (tz_group_debugging)
      tz_group_forget_unstored(a);
    tz_free(a->alloc);
    tz_free(a);
    --tz_live_arrays;
    if (dead.count == 0)
      break;
    a = dead.items[--dead.count];
  }
  tz_free(dead.items);
}

void tz_arr_drop_member(TzArray *a) {
  DeadArrays dead = {NULL, 0, 0};
  if (tz_is_managed(a->type))
    for (uint32_t i = 0; i < a->length; ++i)
      release_value(a->type, element(a, i), &dead, a);
  tz_free(a->alloc);
  a->alloc = NULL;
  a->length = 0;
  while (dead.count > 0)
    tz_arr_free(dead.items[--dead.count]);
  tz_free(dead.items);
}

void tz_value_release(const TzType *type, void *value) {
  DeadArrays dead = {NULL, 0, 0};
  release_value(type, value, &dead, NULL);
  while (dead.count > 0)
    tz_arr_free(dead.items[--dead.count]);
  tz_free(dead.items);
}

void tz_value_release_in(void *owner, const TzType *type, void *value) {
  DeadArrays dead = {NULL, 0, 0};
  release_value(type, value, &dead, ((TzObject *)owner)->rc < 0 ? owner : NULL);
  while (dead.count > 0)
    tz_arr_free(dead.items[--dead.count]);
  tz_free(dead.items);
}

// Releases elements from..to of `a`, which it holds no more.
static void release_range(TzArray *a, uint32_t from, uint32_t to) {
  if (!tz_is_managed(a->type))
    return;
  DeadArrays dead = {NULL, 0, 0};
  void *owner = a->rc < 0 ? a : NULL;
  for (uint32_t i = from; i < to; ++i)
    release_value(a->type, element(a, i), &dead, owner);
  while (dead.count > 0)
    tz_arr_free(dead.items[--dead.count]);
  tz_free(dead.items);
}

// The elements from..to of `a` were copied in: each gets counted, and in a
// member of a group joins it.
static void retain_range(TzArray *a, uint32_t from, uint32_t to) {
  if (!tz_is_managed(a->type))
    return;
  for (uint32_t i = from; i < to; ++i)
    tz_value_retain(a->type, element(a, i));
  if (a->rc < 0)
    for (uint32_t i = from; i < to; ++i)
      tz_group_store_value(a, a->type, element(a, i));
}

// The same for `n` values of `type` stored from `values` on.
static void retain_range_of(const TzType *type, char *values, uint32_t n) {
  if (tz_is_managed(type))
    for (uint32_t i = 0; i < n; ++i)
      tz_value_retain(type, values + (size_t)i * type->size);
}

//===----------------------------------------------------------------------===//
// Creation and storage
//===----------------------------------------------------------------------===//

// A new array of `length` zero elements (0, false, "", undefined) with room
// for `capacity`, in one allocation.
TzArray *tz_arr_new(const TzType *type, int64_t length, int64_t capacity) {
  if (capacity < length)
    capacity = length;
  if (length < 0 || capacity > MAX_LENGTH)
    invalid_length(length < 0 ? length : capacity);
  TzArray *a = (TzArray *)tz_alloc(sizeof(TzArray) + (size_t)capacity * type->size);
  a->rc = 1;
  a->type = type;
  a->data = inline_storage(a);
  a->length = (uint32_t)length;
  a->capacity = (uint32_t)capacity;
  a->alloc = NULL;
  if (length)
    memset(a->data, 0, (size_t)length * type->size);
  ++tz_live_arrays;
  return a;
}

TzArray *tz_arr_new_group(const TzType *type, int64_t length, int64_t capacity) {
  if (capacity < length)
    capacity = length;
  if (length < 0 || capacity > MAX_LENGTH)
    invalid_length(length < 0 ? length : capacity);
  TzArray *a = (TzArray *)tz_group_alloc_tagged(sizeof(TzArray) + (size_t)capacity * type->size,
                                                TZ_GROUP_ARRAY);
  a->type = type;
  a->data = inline_storage(a);
  a->length = (uint32_t)length;
  a->capacity = (uint32_t)capacity;
  a->alloc = NULL;
  if (length)
    memset(a->data, 0, (size_t)length * type->size);
  ++tz_live_arrays;
  return a;
}

// A new array of `a`'s elements: a member of a group if `a` is one, as a copy
// of an array that may form cycles may too.
static TzArray *new_like(const TzArray *a, int64_t length) {
  return a->rc < 0 ? tz_arr_new_group(a->type, length, length) : tz_arr_new(a->type, length, length);
}

// `new Array<T>(n)`
// `new Array(n)`. A negative length throws RangeError, as in JavaScript (the
// program checks after the call, and takes nothing then).
TzArray *tz_arr_new_length(const TzType *type, int64_t length) {
  if (length < 0) {
    tz_throw(TZ_RANGE_ERROR, "Invalid array length");
    return NULL;
  }
  if (length > MAX_LENGTH)
    invalid_length(length);
  return tz_arr_new(type, length, length);
}

TzArray *tz_arr_new_length_group(const TzType *type, int64_t length) {
  if (length < 0) {
    tz_throw(TZ_RANGE_ERROR, "Invalid array length");
    return NULL;
  }
  if (length > MAX_LENGTH)
    invalid_length(length);
  return tz_arr_new_group(type, length, length);
}

// Moves the elements to a new buffer with `front` free slots before them and
// room for `capacity` elements from the first one on.
static void reallocate(TzArray *a, uint32_t front, uint32_t capacity) {
  size_t size = a->type->size;
  char *buffer = (char *)tz_alloc(((size_t)front + capacity) * size);
  char *data = buffer + (size_t)front * size;
  if (a->length)
    memcpy(data, a->data, (size_t)a->length * size);
  tz_free(a->alloc);
  a->alloc = buffer;
  a->data = data;
  a->capacity = capacity;
}

// Makes room for at least `min` elements, growing by half as V8 does.
TZ_COLD void tz_arr_grow(TzArray *a, int64_t min) {
  if (min > MAX_LENGTH)
    invalid_length(min);
  uint64_t capacity = (uint64_t)a->length + a->length / 2 + 8;
  if (capacity < (uint64_t)min)
    capacity = (uint64_t)min;
  if (capacity > MAX_LENGTH)
    capacity = MAX_LENGTH;
  reallocate(a, 0, (uint32_t)capacity);
}

// Moves the first element to `out` (zeroes it if the array is empty) and
// returns whether there was one.
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
    // Empty again: start over at the beginning of the storage.
    char *base = storage(a);
    a->capacity += (uint32_t)((size_t)(a->data - base) / size);
    a->data = base;
  }
  return 1;
}

void tz_arr_push_value(TzArray *a, const void *value) {
  if (a->length == a->capacity)
    tz_arr_grow(a, (int64_t)a->length + 1);
  memcpy(element(a, a->length), value, a->type->size);
  if (a->rc < 0)
    tz_group_store_value(a, a->type, element(a, a->length));
  ++a->length;
}

// Makes `n` slots at the front for unshift() and returns the first one. The
// compiler stores the new elements there.
void *tz_arr_unshift_slots(TzArray *a, int64_t n) {
  if (n == 0)
    return a->data;
  size_t size = a->type->size;
  uint64_t length = (uint64_t)a->length + (uint64_t)n;
  if (length > MAX_LENGTH)
    invalid_length((int64_t)length);
  uint64_t front = (uint64_t)(a->data - storage(a)) / size;
  if (front >= (uint64_t)n) {
    a->data -= (size_t)n * size;
    a->capacity += (uint32_t)n;
    a->length = (uint32_t)length;
    return a->data;
  }
  // A new buffer with spare room at both ends, so that both unshift() and
  // push() stay amortized O(1).
  uint64_t spare = length / 2 + 8;
  uint64_t newFront = spare / 2;
  uint64_t capacity = length + (spare - newFront);
  if (capacity > MAX_LENGTH)
    capacity = length;
  char *buffer = (char *)tz_alloc((size_t)(newFront + capacity) * size);
  char *data = buffer + (size_t)newFront * size;
  if (a->length)
    memcpy(data + (size_t)n * size, a->data, (size_t)a->length * size);
  tz_free(a->alloc);
  a->alloc = buffer;
  a->data = data;
  a->capacity = (uint32_t)capacity;
  a->length = (uint32_t)length;
  return data;
}

// `a.length = n`: drops elements, or adds zero ones. Arrays, functions and
// objects have no zero value: an array of them can only get shorter.
void tz_arr_set_length(TzArray *a, int64_t n) {
  // RangeErrors, which the program checks for after the call.
  if (n < 0) {
    tz_throw(TZ_RANGE_ERROR, "Invalid array length");
    return;
  }
  if (n > MAX_LENGTH)
    invalid_length(n);
  size_t size = a->type->size;
  uint32_t old = a->length;
  uint32_t kind = a->type->kind;
  if ((uint64_t)n > old && (kind == TZ_T_ARRAY || kind == TZ_T_FUNCTION || kind == TZ_T_OBJECT ||
                            kind == TZ_T_JSON)) {
    tz_throw(TZ_RANGE_ERROR,
             "an array of %s cannot grow by setting its length, which would leave elements "
             "without a value; use push()",
             kind == TZ_T_ARRAY      ? "arrays"
             : kind == TZ_T_FUNCTION ? "functions"
             : kind == TZ_T_JSON     ? "Json values"
                                     : "objects");
    return;
  }
  if ((uint64_t)n < old) {
    a->length = (uint32_t)n;
    release_range(a, (uint32_t)n, old);
  } else if ((uint64_t)n > old) {
    if ((uint64_t)n > a->capacity)
      tz_arr_grow(a, n);
    memset(element(a, old), 0, ((size_t)n - old) * size);
    a->length = (uint32_t)n;
  }
}

//===----------------------------------------------------------------------===//
// Methods
//===----------------------------------------------------------------------===//

// A relative position as slice() and fill() take it: negative counts from the
// end; the result is clamped to [0, length].
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

// A slice and a copy of an array of a type that may form cycles, as the
// compiler wants them: members of groups, or not when it found that they
// are never stored (see StoreScanner in src/codegen/codegen_cycles.cpp),
// whatever `a` is.
static TzArray *slice_as(TzArray *a, int64_t start, int64_t end, int32_t has_end, int member) {
  int64_t length = a->length;
  int64_t from = relative(start, length);
  int64_t to = has_end ? relative(end, length) : length;
  int64_t count = to > from ? to - from : 0;
  TzArray *result = member ? tz_arr_new_group(a->type, count, count) : tz_arr_new(a->type, count, count);
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

// splice(start, deleteCount, ...items): moves the removed elements into a new
// array, which it returns, and opens a gap of `insert` slots where they were;
// `*slots` gets its address, and the compiler stores the items there.
TzArray *tz_arr_splice(TzArray *a, int64_t start, int64_t delete_count, int32_t has_delete,
                       int64_t insert, void **slots) {
  int64_t length = a->length;
  int64_t from = relative(start, length);
  int64_t count = length - from;
  if (has_delete && delete_count < count)
    count = delete_count < 0 ? 0 : delete_count;
  size_t size = a->type->size;
  TzArray *removed = tz_arr_new(a->type, count, count);
  if (count) {
    memcpy(removed->data, element(a, (uint32_t)from), (size_t)count * size);
    // They move out of a member to an array of their own, which is none.
    if (a->rc < 0)
      for (int64_t i = 0; i < count; ++i)
        tz_group_take_value(a, a->type, removed->data + (size_t)i * size);
  }
  int64_t new_length = length - count + insert;
  if (new_length > MAX_LENGTH)
    invalid_length(new_length);
  if (insert != count) {
    if (new_length > a->capacity)
      tz_arr_grow(a, new_length);
    memmove(element(a, (uint32_t)(from + insert)), element(a, (uint32_t)(from + count)),
            (size_t)(length - from - count) * size);
  }
  a->length = (uint32_t)new_length;
  *slots = element(a, (uint32_t)from);
  return removed;
}

// Appends the elements of `src` to `dst` (for spreads and concat()).
void tz_arr_append(TzArray *dst, TzArray *src) {
  uint32_t n = src->length;
  if (n == 0)
    return;
  uint64_t total = (uint64_t)dst->length + n;
  if (total > dst->capacity)
    tz_arr_grow(dst, (int64_t)total);
  // Read src->data only now: src may be dst, which just moved.
  memcpy(element(dst, dst->length), src->data, (size_t)n * dst->type->size);
  uint32_t from = dst->length;
  dst->length = (uint32_t)total;
  retain_range(dst, from, dst->length);
}

// ===, or SameValueZero (NaN equals NaN) for includes().
static int values_equal(const TzType *type, const void *a, const void *b, int same_value_zero) {
  switch (type->kind) {
  case TZ_T_ATOM:
    return *(const int32_t *)a == *(const int32_t *)b;
  case TZ_T_UNION: { // the same member, equal
    uint32_t tag = *(const uint32_t *)a;
    if (tag != *(const uint32_t *)b)
      return 0;
    if (tag == 0) // both undefined
      return 1;
    const void *x = a, *y = b;
    tz_resolve(type, &y);
    type = tz_resolve(type, &x);
    return values_equal(type, x, y, same_value_zero);
  }
  case TZ_T_INT:
    return *(const int32_t *)a == *(const int32_t *)b;
  case TZ_T_LONG:
    return *(const int64_t *)a == *(const int64_t *)b;
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
  case TZ_T_OBJECT: // the same one
    return *(void *const *)a == *(void *const *)b;
  case TZ_T_JSON: // as `===` compares the values JSON.parse gives
    return tz_json_strict_equal(*(TzJson *const *)a, *(TzJson *const *)b);
  case TZ_T_UNKNOWN: // as `===` compares what they hold
    return tz_unknown_equal(*(void *const *)a, *(void *const *)b);
  case TZ_T_DECIMAL: { // the same value
    const TzDec *x = (const TzDec *)a, *y = (const TzDec *)b;
    return tz_dec_equal(x->coef, x->meta, y->coef, y->meta);
  }
  case TZ_T_FUNCTION: {
    const TzFunction *x = (const TzFunction *)a, *y = (const TzFunction *)b;
    return x->info == y->info && x->env == y->env;
  }
  case TZ_T_OPTIONAL: {
    int has_a = tz_optional_has(type, a), has_b = tz_optional_has(type, b);
    if (!has_a || !has_b)
      return has_a == has_b;
    return values_equal(type->inner, a, b, same_value_zero);
  }
  default:
    return 0;
  }
}

// indexOf (mode 0), lastIndexOf (mode 1) and includes (mode 2, returns 0/1).
int32_t tz_arr_index_of(TzArray *a, const void *value, int64_t from, int32_t has_from, int32_t mode) {
  int64_t length = a->length;
  if (mode == 1) {
    int64_t i = !has_from ? length - 1 : from < 0 ? length + from : (from < length ? from : length - 1);
    for (; i >= 0; --i)
      if (values_equal(a->type, element(a, (uint32_t)i), value, 0))
        return (int32_t)i;
    return -1;
  }
  for (int64_t i = has_from ? relative(from, length) : 0; i < length; ++i)
    if (values_equal(a->type, element(a, (uint32_t)i), value, mode == 2))
      return mode == 2 ? 1 : (int32_t)i;
  return mode == 2 ? 0 : -1;
}

void tz_buf_value(TzBuf *out, const TzType *type, const void *value, int in_join) {
  char text[64];
  type = tz_resolve(type, &value);
  switch (type->kind) {
  case TZ_T_INT:
  case TZ_T_LONG: {
    int64_t v = type->kind == TZ_T_INT ? *(const int32_t *)value : *(const int64_t *)value;
    uint64_t magnitude = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
    char *p = text + sizeof text;
    do {
      *--p = (char)('0' + magnitude % 10);
      magnitude /= 10;
    } while (magnitude != 0);
    if (v < 0)
      *--p = '-';
    tz_buf_ascii_n(out, p, (size_t)(text + sizeof text - p));
    break;
  }
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
    // Array.prototype.toString joins with commas, also when nested.
    tz_buf_join(out, *(TzArray *const *)value, NULL);
    break;
  case TZ_T_OBJECT: {
    // Only an Error, a date and a regex have a text of their own (Sema
    // rejects other objects): a date's is its toString(), in local time.
    const TzInstance *object = *(const TzInstance *const *)value;
    if (object && (object->layout->flags & TZ_LAYOUT_ERROR)) {
      tz_buf_error_text(out, object);
    } else if (object && (object->layout->flags & TZ_LAYOUT_REGEX)) {
      tz_buf_regex(out, (const TzRegex *)object);
    } else if (object && (object->layout->flags & TZ_LAYOUT_DATE)) {
      TzStr text;
      tz_date_local_string(&text, ((const TzDate *)object)->time, 0); // TZ_DATE_TEXT_FULL
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
    // fall through: undefined
  case TZ_T_UNDEFINED:
    // join() shows undefined elements as empty.
    if (!in_join)
      tz_buf_ascii(out, "undefined");
    break;
  default:
    break;
  }
}

void tz_buf_join(TzBuf *out, const TzArray *a, const TzBuf *sep) {
  for (uint32_t i = 0; i < a->length; ++i) {
    if (i > 0) {
      if (sep)
        tz_buf_units(out, sep->data, sep->length);
      else
        tz_buf_push(out, ',');
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
  if (has_sep)
    tz_buf_str(&sep, separator);
  tz_buf_join(&buf, a, has_sep ? &sep : NULL);
  tz_buf_free(&sep);
  *out = tz_buf_take(&buf);
}

// String(value) for arrays and optionals (the compiler converts the other
// types inline).
void tz_value_to_string(TzStr *out, const TzType *type, const void *value) {
  type = tz_resolve(type, &value);
  if (type->kind == TZ_T_STRING) {
    *out = *(const TzStr *)value;
    tz_str_retain_value(*out);
    return;
  }
  if (type->kind == TZ_T_OPTIONAL && tz_optional_has(type, value) && type->inner->kind == TZ_T_STRING) {
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
  if (tmp != small)
    tz_free(tmp);
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
    if (managed && a->rc < 0)
      tz_group_store_value(a, a->type, slot);
  }
}

// Sorting: JavaScript's default order compares the string forms of the
// elements by UTF-16 code units, keeps undefined last, and is stable.
typedef struct {
  TzStr key;
  uint32_t index;
  uint8_t undefined;
  uint8_t owned; // the key was made for sorting and must be released
} SortItem;

static int sort_before(const SortItem *a, const SortItem *b) {
  if (a->undefined || b->undefined)
    return !a->undefined && b->undefined;
  return tz_str_compare(a->key.obj, a->key.meta, b->key.obj, b->key.meta) < 0;
}

static void merge_sort(SortItem *items, SortItem *tmp, size_t n) {
  if (n < 2)
    return;
  size_t half = n / 2;
  merge_sort(items, tmp, half);
  merge_sort(items + half, tmp, n - half);
  size_t i = 0, j = half, k = 0;
  while (i < half && j < n)
    tmp[k++] = sort_before(&items[j], &items[i]) ? items[j++] : items[i++];
  while (i < half)
    tmp[k++] = items[i++];
  while (j < n)
    tmp[k++] = items[j++];
  memcpy(items, tmp, n * sizeof(SortItem));
}

// sort(compare): `compare(ctx, x, y)` calls the program's comparison function
// on the elements at x and y and returns the sign of its result (NaN counts
// as 0). Stable, and undefined elements go last without being compared, as
// in JavaScript.
typedef int32_t (*TzCompare)(void *ctx, const void *x, const void *y);

static void merge_sort_with(char *items, char *tmp, size_t n, size_t size, TzCompare compare, void *ctx) {
  if (n < 2)
    return;
  size_t half = n / 2;
  merge_sort_with(items, tmp, half, size, compare, ctx);
  merge_sort_with(items + half * size, tmp, n - half, size, compare, ctx);
  size_t i = 0, j = half, k = 0;
  while (i < half && j < n) {
    // The right one goes first only if it compares strictly before.
    if (compare(ctx, items + i * size, items + j * size) > 0)
      memcpy(tmp + k++ * size, items + j++ * size, size);
    else
      memcpy(tmp + k++ * size, items + i++ * size, size);
  }
  while (i < half)
    memcpy(tmp + k++ * size, items + i++ * size, size);
  while (j < n)
    memcpy(tmp + k++ * size, items + j++ * size, size);
  memcpy(items, tmp, n * size);
}

void tz_arr_sort_with(TzArray *a, TzCompare compare, void *ctx) {
  uint32_t n = a->length;
  if (n < 2)
    return;
  const TzType *type = a->type;
  size_t size = type->size;
  // The elements are sorted in a copy that holds references of its own: the
  // comparison function may change the array meanwhile.
  char *items = (char *)tz_alloc((size_t)n * size);
  char *tmp = (char *)tz_alloc((size_t)n * size);
  size_t defined = 0, undefined = n;
  for (uint32_t i = 0; i < n; ++i) {
    const char *value = element(a, i);
    if (type->kind == TZ_T_OPTIONAL && !tz_optional_has(type, value))
      memcpy(tmp + (size_t)--undefined * size, value, size);
    else
      memcpy(items + defined++ * size, value, size);
  }
  // The undefined ones keep their order, after the others.
  for (size_t i = defined; i < n; ++i)
    memcpy(items + i * size, tmp + (n - 1 - (i - defined)) * size, size);
  retain_range_of(type, items, n);
  merge_sort_with(items, tmp, defined, size, compare, ctx);
  if (a->length == n) {
    release_range(a, 0, n);
    memcpy(a->data, items, (size_t)n * size);
    if (a->rc < 0 && tz_is_managed(type))
      for (uint32_t i = 0; i < n; ++i)
        tz_group_store_value(a, type, element(a, i));
  } else if (tz_is_managed(type)) {
    // The function changed the array's length: JavaScript leaves the result
    // up to the engine; here the array stays as the function left it.
    for (uint32_t i = 0; i < n; ++i)
      tz_value_release(type, items + (size_t)i * size);
  }
  tz_free(items);
  tz_free(tmp);
}

void tz_arr_sort(TzArray *a) {
  uint32_t n = a->length;
  if (n < 2)
    return;
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
      item->key = *(const TzStr *)value; // borrowed from the array
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
  for (uint32_t k = 0; k < n; ++k)
    memcpy(sorted + (size_t)k * size, element(a, items[k].index), size);
  memcpy(a->data, sorted, (size_t)n * size);
  tz_free(sorted);
  for (uint32_t i = 0; i < n; ++i)
    if (items[i].owned)
      tz_str_release_value(items[i].key);
  tz_free(items);
  tz_free(tmp);
}
