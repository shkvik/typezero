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

TzJson *tz_json_null(void) { return tz_json_retained(&tz_json_null_node); }

TzJson *tz_json_bool(int32_t value) {
  return tz_json_retained(value ? &tz_json_true_node : &tz_json_false_node);
}

TzJson *tz_json_number(double value) {
  TzJson *j = tz_json_node_new(TZ_JSON_NUMBER, sizeof(double));
  j->u.number.value = value;

  return j;
}

TzJson *tz_json_string_node(TzStr s) {
  TzJson *j = tz_json_node_new(TZ_JSON_STRING, sizeof(TzStr));
  j->u.string = s;

  return j;
}

TzJson *tz_json_string(void *obj, uint64_t meta) {
  TzStr s = {obj, meta};
  tz_str_retain_value(s);

  return tz_json_string_node(s);
}

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

void tz_json_object_put(TzJson *o, void *key_obj, uint64_t key_meta, TzJson *value) {
  TzStr key = {key_obj, key_meta};
  tz_json_put(o, key, value);
}

int32_t tz_json_kind(const TzJson *j) { return j ? (int32_t)j->kind : -1; }

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

TZ_HOT int32_t tz_json_is_nullish(const TzJson *j) { return !j || j->kind == TZ_JSON_NULL; }

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
    return a->u.number.value == b->u.number.value;
  case TZ_JSON_STRING:
    return tz_json_key_equal(a->u.string, b->u.string);
  case TZ_JSON_ARRAY:
    return a->u.array == b->u.array;
  default:
    return a == b;
  }
}

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

TzJson *tz_json_get(const TzJson *j, void *key_obj, uint64_t key_meta) {
  TzStr key = {key_obj, key_meta};
  if (!j || j->kind == TZ_JSON_NULL) {
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
  default:
    return NULL;
  }
}

static TzStr number_key(double index) {
  TzStr key;
  tz_str_from_f64(&key, index);

  return key;
}

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

static void set_element(const TzJson *j, int64_t i, TzJson *value) {
  TzArray *a = j->u.array;
  if (!value) {
    value = tz_json_null();
  }
  while (i > a->length) {
    TzJson *hole = tz_json_null();
    tz_arr_push_value(a, &hole);
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

static int64_t key_count(const TzJson *j) {
  if (!j || j->kind == TZ_JSON_NULL) {
    return 0;
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
  uint32_t *order = j && j->kind == TZ_JSON_OBJECT ? tz_json_key_order(j) : NULL;
  TzArray *keys = tz_arr_new(string_type, 0, count);
  for (uint32_t k = 0; k < count; ++k) {
    TzStr key;
    entry_at(j, order, k, &key, NULL);
    tz_arr_push_value(keys, &key);
  }
  tz_free(order);

  return keys;
}

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
  uint32_t *order = j && j->kind == TZ_JSON_OBJECT ? tz_json_key_order(j) : NULL;
  TzArray *values = tz_arr_new(&tz_json_type, 0, count);
  for (uint32_t k = 0; k < count; ++k) {
    TzJson *value;
    entry_at(j, order, k, NULL, &value);
    tz_arr_push_value(values, &value);
  }
  tz_free(order);

  return values;
}

TzArray *tz_json_entries(const TzJson *j, const TzType *tuple_type, const TzLayout *tuple_layout,
                         int64_t tuple_size) {
  int64_t count = key_count(j);
  if (count < 0) {
    return NULL;
  }
  uint32_t *order = j && j->kind == TZ_JSON_OBJECT ? tz_json_key_order(j) : NULL;
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
