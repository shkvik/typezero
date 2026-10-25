// Json values as the program makes, reads and writes them, and Object.keys,
// Object.values and Object.entries of them.

#include "runtime/library/json/json_values/json_values.h"
#include "runtime/library/json/json/json.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/array/array.h"
#include "runtime/core/closure/closure.h"
#include "runtime/core/error/error.h"
#include "runtime/core/group/group.h"
#include "runtime/core/output/output.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include <math.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//===----------------------------------------------------------------------===//
// Making values (Lowering: literals and conversions)
//===----------------------------------------------------------------------===//

TzJson *tz_json_null(void) { return tz_json_retained(&tz_json_null_node); }

TzJson *tz_json_bool(int32_t value) {
  return tz_json_retained(value ? &tz_json_true_node : &tz_json_false_node);
}

TzJson *tz_json_number(double value) {
  TzJson *j = tz_json_node_new(TZ_JSON_NUMBER, sizeof(double));
  j->u.number.value = value;

  return j;
}

TzJson *tz_json_string_node(TzStr s) { // takes over s
  TzJson *j = tz_json_node_new(TZ_JSON_STRING, sizeof(TzStr));
  j->u.string = s;

  return j;
}

// The string is borrowed.
TzJson *tz_json_string(void *obj, uint64_t meta) {
  TzStr s = {obj, meta};
  tz_str_retain_value(s);

  return tz_json_string_node(s);
}

// Takes over the reference to `array`, an array of Json.
TzJson *tz_json_from_array(TzArray *array) {
  TzJson *j = tz_json_node_new(TZ_JSON_ARRAY, sizeof(TzArray *));
  j->u.array = array;

  return j;
}

TzJson *tz_json_array_node(uint32_t capacity) {
  return tz_json_from_array(tz_arr_new(&tz_json_type, 0, capacity));
}

TzJson *tz_json_object_new(void) {
  TzJson *j = tz_json_node_new(TZ_JSON_OBJECT, sizeof(j->u.object));
  j->u.object.entries = NULL;
  j->u.object.count = j->u.object.capacity = 0;
  j->u.object.index = NULL;
  j->u.object.index_size = 0;

  return j;
}

// `{ key: value }` of a literal: the key is borrowed, the value taken over.
void tz_json_object_put(TzJson *o, void *key_obj, uint64_t key_meta, TzJson *value) {
  TzStr key = {key_obj, key_meta};
  tz_json_put(o, key, value);
}

int32_t tz_json_kind(const TzJson *j) { return j ? (int32_t)j->kind : -1; }

// `typeof j`: 0 "undefined", 1 "object" (null too, as in JavaScript), 2
// "boolean", 3 "number", 4 "string".
int32_t tz_json_typeof(const TzJson *j) {
  if (!j) {
    return 0;
  }
  switch (j->kind) {
  case TZ_JSON_BOOL:
    return 2;
  case TZ_JSON_NUMBER:
    return 3;
  case TZ_JSON_STRING:
    return 4;
  default:
    return 1;
  }
}

// `j == null` and `j == undefined`, which are true for both.
TZ_HOT int32_t tz_json_is_nullish(const TzJson *j) { return !j || j->kind == TZ_JSON_NULL; }

// The value of a Json known to be a number, a boolean, a string or an array
// (Checker narrowed it with typeof or Array.isArray). The string and the array
// are owned.
double tz_json_as_number(const TzJson *j) {
  return j && j->kind == TZ_JSON_NUMBER ? j->u.number.value : 0;
}

int32_t tz_json_as_bool(const TzJson *j) { return j && j->kind == TZ_JSON_BOOL && j->u.boolean; }

void tz_json_as_string(TzStr *out, const TzJson *j) {
  if (!j || j->kind != TZ_JSON_STRING) {
    *out = tz_str_from_ascii("", 0);
    return;
  }
  *out = j->u.string;
  tz_str_retain_value(*out);
}

TzArray *tz_json_as_array(const TzJson *j) {
  if (!j || j->kind != TZ_JSON_ARRAY) {
    return tz_arr_new(&tz_json_type, 0, 0);
  }
  tz_rc_inc(&j->u.array->rc);

  return j->u.array;
}

int32_t tz_json_strict_equal(const TzJson *a, const TzJson *b) {
  if (!a || !b) {
    return a == b;
  }
  if (a->kind != b->kind) {
    return 0;
  }
  switch (a->kind) {
  case TZ_JSON_NULL:
    return 1;
  case TZ_JSON_BOOL:
    return a->u.boolean == b->u.boolean;
  case TZ_JSON_NUMBER:
    return a->u.number.value == b->u.number.value; // NaN is not NaN
  case TZ_JSON_STRING:
    return tz_json_key_equal(a->u.string, b->u.string);
  case TZ_JSON_ARRAY:
    return a->u.array == b->u.array; // the same array
  default:
    return a == b;
  }
}

// `j == x` with a number, a string or a boolean x: as `===`, true only for a
// Json of that kind and value.
int32_t tz_json_equals_number(const TzJson *j, double value) {
  return j && j->kind == TZ_JSON_NUMBER && j->u.number.value == value;
}

int32_t tz_json_equals_string(const TzJson *j, void *obj, uint64_t meta) {
  return j && j->kind == TZ_JSON_STRING &&
         tz_str_equal(j->u.string.obj, j->u.string.meta, obj, meta);
}

int32_t tz_json_equals_bool(const TzJson *j, int32_t value) {
  return j && j->kind == TZ_JSON_BOOL && j->u.boolean == (value != 0);
}

const char *tz_json_kind_name(const TzJson *j) {
  if (!j) {
    return "undefined";
  }
  switch (j->kind) {
  case TZ_JSON_NULL:
    return "null";
  case TZ_JSON_BOOL:
    return "boolean";
  case TZ_JSON_NUMBER:
    return "number";
  case TZ_JSON_STRING:
    return "string";
  case TZ_JSON_ARRAY:
    return "array";
  default:
    return "object";
  }
}

// `j.key` of undefined or null: the TypeError of JavaScript.
static void read_error(const TzJson *j, TzStr key, const char *verb) {
  TzBuf b;
  tz_buf_init(&b);
  tz_buf_ascii(&b, "Cannot ");
  tz_buf_ascii(&b, verb);
  tz_buf_ascii(&b, " properties of ");
  tz_buf_ascii(&b, tz_json_kind_name(j));
  tz_buf_ascii(&b, " (");
  tz_buf_ascii(&b, verb[0] == 'r' ? "reading '" : "setting '");
  tz_buf_str(&b, key);
  tz_buf_ascii(&b, "')");
  tz_json_throw(TZ_TYPE_ERROR, &b);
}

// The element `i` of an array, or the code unit `i` of a string, as a Json;
// NULL past the end.
static TzJson *element_of(const TzJson *j, uint32_t i) {
  if (j->kind == TZ_JSON_ARRAY) {
    const TzArray *a = j->u.array;
    return i < a->length ? tz_json_retained(((TzJson **)a->data)[i]) : NULL;
  }
  TzStr s = j->u.string;
  View v;
  tz_json_view_init(&v, s);
  if (i >= v.length) {
    return NULL;
  }
  uint16_t unit = tz_json_view_at(&v, i);

  return tz_json_string_node(tz_str_from_utf16(&unit, 1));
}

// `j.key` and `j["key"]` as JavaScript reads them: the value of the key of an
// object, an element or the length of an array or a string, undefined (NULL)
// for anything else. Reading from undefined or null is a TypeError (and
// gives NULL). The object is borrowed, the result owned.
TzJson *tz_json_get(const TzJson *j, void *key_obj, uint64_t key_meta) {
  TzStr key = {key_obj, key_meta};
  if (!j || j->kind == TZ_JSON_NULL) {
    read_error(j, key, "read");
    return NULL;
  }
  switch (j->kind) {
  case TZ_JSON_OBJECT: {
    int64_t at = tz_json_object_find(j, key);
    return at >= 0 ? tz_json_retained(j->u.object.entries[at].value) : NULL;
  }
  case TZ_JSON_ARRAY:
  case TZ_JSON_STRING: {
    int64_t index = tz_json_index_key(key);
    if (index >= 0) {
      return element_of(j, (uint32_t)index);
    }
    if (!tz_json_is_length_key(key)) {
      return NULL;
    }
    if (j->kind == TZ_JSON_ARRAY) {
      return tz_json_number(j->u.array->length);
    }
    View v;
    tz_json_view_init(&v, j->u.string);
    return tz_json_number(v.length);
  }
  default: // numbers and booleans have no keys of their own
    return NULL;
  }
}

// The key a number names, as JavaScript writes it: `j[1.5]` reads "1.5".
static TzStr number_key(double index) {
  TzStr key;
  tz_str_from_f64(&key, index);

  return key;
}

// `j[i]` with a number i.
TzJson *tz_json_get_index(const TzJson *j, double index) {
  int whole = index >= 0 && index <= 4294967294.0 && index == floor(index);
  if (whole && j && (j->kind == TZ_JSON_ARRAY || j->kind == TZ_JSON_STRING)) {
    return element_of(j, (uint32_t)index);
  }
  TzStr key = number_key(index);
  TzJson *result = tz_json_get(j, key.obj, key.meta);
  tz_str_release_value(key);

  return result;
}

// `j[i] = value` of an array: in place, or at the end. Further on JavaScript
// would leave holes, which arrays do not have.
static void set_element(const TzJson *j, int64_t i, TzJson *value) {
  TzArray *a = j->u.array;
  if (!value || i > a->length) {
    tz_json_release(value);
    TzBuf b;
    tz_buf_init(&b);
    char text[160];
    int n = value ? snprintf(text, sizeof text,
                             "Cannot set element %lld of a Json array of length %u: that would "
                             "leave holes; use push() on it as Json[]",
                             (long long)i, a->length)
                  : snprintf(text, sizeof text,
                             "Cannot set element %lld of a Json array to undefined", (long long)i);
    tz_buf_ascii_n(&b, text, (size_t)n);
    tz_json_throw(TZ_TYPE_ERROR, &b);
    return;
  }
  if (i == a->length) {
    tz_arr_push_value(a, &value);
    return;
  }
  TzJson **items = (TzJson **)a->data;
  TzJson *old = items[i];
  items[i] = value;
  tz_json_release(old);
}

// `j.key = value` and `j["key"] = value`; the value is taken over, and
// undefined (NULL) removes the key: a key that holds undefined and one that
// is not there are the same. Only objects and arrays (at an index up to their
// length) can be written to; the rest is the TypeError of JavaScript.
void tz_json_set(const TzJson *j, void *key_obj, uint64_t key_meta, TzJson *value) {
  TzStr key = {key_obj, key_meta};
  if (j && j->kind == TZ_JSON_OBJECT) {
    if (value) {
      tz_json_put((TzJson *)j, key, value);
    } else {
      tz_json_object_remove((TzJson *)j, key);
    }
    return;
  }
  int64_t index = j && j->kind == TZ_JSON_ARRAY ? tz_json_index_key(key) : -1;
  if (index >= 0) {
    set_element(j, index, value);
    return;
  }
  tz_json_release(value);
  if (!j || j->kind == TZ_JSON_NULL) {
    read_error(j, key, "set");
    return;
  }
  TzBuf b;
  tz_buf_init(&b);
  tz_buf_ascii(&b, "Cannot create property '");
  tz_buf_str(&b, key);
  tz_buf_ascii(&b, "' on ");
  if (j->kind == TZ_JSON_ARRAY) {
    tz_buf_ascii(&b, "a Json array, which only has elements");
  } else {
    tz_buf_ascii(&b, tz_json_kind_name(j));
    tz_buf_ascii(&b, " '");
    if (j->kind == TZ_JSON_STRING) {
      tz_buf_str(&b, j->u.string);
    } else if (j->kind == TZ_JSON_BOOL) {
      tz_buf_ascii(&b, j->u.boolean ? "true" : "false");
    } else {
      char text[32];
      tz_buf_ascii_n(&b, text, tz_format_f64(j->u.number.value, text));
    }
    tz_buf_push(&b, '\'');
  }
  tz_json_throw(TZ_TYPE_ERROR, &b);
}

void tz_json_set_index(const TzJson *j, double index, TzJson *value) {
  int whole = index >= 0 && index <= 4294967294.0 && index == floor(index);
  if (whole && j && j->kind == TZ_JSON_ARRAY) {
    set_element(j, (int64_t)index, value);
    return;
  }
  TzStr key = number_key(index);
  tz_json_set(j, key.obj, key.meta, value);
  tz_str_release_value(key);
}

//===----------------------------------------------------------------------===//
// Object.keys, Object.values and Object.entries
//===----------------------------------------------------------------------===//

// The keys of a Json and their values, as JavaScript gives them for any
// value: an object's in the order of its keys, the indices of an array or a
// string, nothing for a number or a boolean. Undefined and null are the
// TypeError of JavaScript, and -1.
static int64_t key_count(const TzJson *j) {
  if (!j || j->kind == TZ_JSON_NULL) {
    TzBuf b;
    tz_buf_init(&b);
    tz_buf_ascii(&b, "Cannot convert undefined or null to object");
    tz_json_throw(TZ_TYPE_ERROR, &b);
    return -1;
  }
  switch (j->kind) {
  case TZ_JSON_OBJECT:
    return j->u.object.count;
  case TZ_JSON_ARRAY:
    return j->u.array->length;
  case TZ_JSON_STRING: {
    View v;
    tz_json_view_init(&v, j->u.string);
    return v.length;
  }
  default:
    return 0;
  }
}

// The key and the value at `k` in the order of the keys (owned).
static void entry_at(const TzJson *j, const uint32_t *order, uint32_t k, TzStr *key,
                     TzJson **value) {
  if (j->kind == TZ_JSON_OBJECT) {
    const TzJsonEntry *e = &j->u.object.entries[order ? order[k] : k];
    if (key) {
      *key = e->key;
      tz_str_retain_value(*key);
    }
    if (value) {
      *value = tz_json_retained(e->value);
    }
    return;
  }
  if (key) {
    *key = tz_json_index_text(k);
  }
  if (value) {
    *value = element_of(j, k);
  }
}

TzArray *tz_json_keys(const TzJson *j, const TzType *string_type) {
  int64_t count = key_count(j);
  if (count < 0) {
    return NULL;
  }
  uint32_t *order = j->kind == TZ_JSON_OBJECT ? tz_json_key_order(j) : NULL;
  TzArray *keys = tz_arr_new(string_type, 0, count);
  for (uint32_t k = 0; k < count; ++k) {
    TzStr key;
    entry_at(j, order, k, &key, NULL);
    tz_arr_push_value(keys, &key);
  }
  tz_free(order);

  return keys;
}

// for...in, which walks no keys of undefined or null.
TzArray *tz_json_for_in_keys(const TzJson *j, const TzType *string_type) {
  if (!j || j->kind == TZ_JSON_NULL) {
    return tz_arr_new(string_type, 0, 0);
  }

  return tz_json_keys(j, string_type);
}

TzArray *tz_json_values(const TzJson *j) {
  int64_t count = key_count(j);
  if (count < 0) {
    return NULL;
  }
  uint32_t *order = j->kind == TZ_JSON_OBJECT ? tz_json_key_order(j) : NULL;
  TzArray *values = tz_arr_new(&tz_json_type, 0, count);
  for (uint32_t k = 0; k < count; ++k) {
    TzJson *value;
    entry_at(j, order, k, NULL, &value);
    tz_arr_push_value(values, &value);
  }
  tz_free(order);

  return values;
}

// Each entry is a new tuple [key, value], an object of `tuple_size` bytes
// described by `tuple_layout`, as in tz_obj_entries.
TzArray *tz_json_entries(const TzJson *j, const TzType *tuple_type, const TzLayout *tuple_layout,
                         int64_t tuple_size) {
  int64_t count = key_count(j);
  if (count < 0) {
    return NULL;
  }
  uint32_t *order = j->kind == TZ_JSON_OBJECT ? tz_json_key_order(j) : NULL;
  TzArray *entries = tz_arr_new(tuple_type, 0, count);
  const TzField *key = &tuple_layout->fields[0], *value = &tuple_layout->fields[1];
  for (uint32_t k = 0; k < count; ++k) {
    TzInstance *entry = (TzInstance *)tz_obj_alloc(tuple_size);
    entry->rc = 1;
    entry->layout = tuple_layout;
    entry_at(j, order, k, (TzStr *)((char *)entry + key->offset),
             (TzJson **)((char *)entry + value->offset));
    tz_arr_push_value(entries, &entry);
  }
  tz_free(order);

  return entries;
}
