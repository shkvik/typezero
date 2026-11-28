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

