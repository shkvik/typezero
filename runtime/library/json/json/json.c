// Json values (docs/modules.md, section 5): what JSON.parse and embedJson
// give, reading and writing their keys, JSON.stringify of any value, and the
// checks behind JSON.parse<T> and fromJson<T> (docs/narrowing.md, section 3).
//
// A node (TzJson in tzrt.h) is counted like the objects of the language. A
// number, a string and an array take only the part of the node they use. An
// object keeps its entries in the order the keys were added, with a hash
// index of the keys once it has more than a few; JavaScript gives the keys
// that are array indices first, so tz_json_key_order makes that order for
// printing, JSON.stringify and Object.keys.
//
// Errors throw what node would throw (see json_throw): the function goes on
// to give nothing, having released what it built, and the program checks
// tz_thrown after the call. JSON.parse and JSON.stringify, which find their
// errors deep in their work, jump back to where they started.

#include "runtime/core/tzrt/tzrt.h"
#include <math.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// TzJson::flags of an object: some key is an array index, so the keys do not
// come in the order they were added.
#define F_INDEX_KEYS 1u
// Of a number: u.number.text holds the text it was read from.
#define F_TEXT 2u

// An object with more keys than this finds them through a hash index.
#define INDEX_FROM 8

// null, true and false are one node each, never freed: their counts start so
// high that they never get back to zero.
#define IMMORTAL (INT64_C(1) << 62)

// Not static: see the rules in tzrt.h.
TzJson tz_json_null_node = {IMMORTAL, TZ_JSON_NULL, 0, {0}};
TzJson tz_json_true_node = {IMMORTAL, TZ_JSON_BOOL, 0, {1}};
TzJson tz_json_false_node = {IMMORTAL, TZ_JSON_BOOL, 0, {0}};

const TzType tz_json_type = {TZ_T_JSON, sizeof(TzJson *), NULL};

int64_t tz_json_live_objects(void) { return tz_thread_sum(offsetof(TzThread, live_json)); }

//===----------------------------------------------------------------------===//
// Nodes and reference counts
//===----------------------------------------------------------------------===//

static TzJson *node_new(uint32_t kind, size_t payload) {
  TzJson *j = (TzJson *)tz_alloc(offsetof(TzJson, u) + payload);
  j->rc = 1;
  j->kind = kind;
  j->flags = 0;
  ++tz_live_json;

  return j;
}

static TzJson *retained(TzJson *j) {
  tz_rc_inc(&j->rc);

  return j;
}

// Nodes whose count reached zero, freed one by one: a deep value is freed
// without recursion.
typedef struct {
  TzJson **items;
  size_t count, capacity;
} Dead;

static void dead_push(Dead *dead, TzJson *j) {
  if (dead->count == dead->capacity) {
    size_t capacity = dead->capacity * 2 + 16;
    TzJson **items = (TzJson **)tz_alloc(capacity * sizeof(TzJson *));
    if (dead->count) {
      memcpy(items, dead->items, dead->count * sizeof(TzJson *));
    }
    tz_free(dead->items);
    dead->items = items;
    dead->capacity = capacity;
  }
  dead->items[dead->count++] = j;
}

static void drop_child(Dead *dead, TzJson *j) {
  if (j && tz_rc_dec(&j->rc)) {
    dead_push(dead, j);
  }
}

#if TZ_PARALLEL
// publish.c: a Json other threads may reach, and everything in it, shared;
// as tz_json_free walks it, without recursion.
void tz_json_publish(TzJson *root) {
  if (!root) {
    return;
  }
  Dead pending = {NULL, 0, 0};
  dead_push(&pending, root);
  while (pending.count) {
    TzJson *j = pending.items[--pending.count];
    if (j->rc <= 0 || j->rc >= TZ_RC_FOREVER) {
      continue; // shared already, or one of the constants
    }
    j->rc = TZ_RC_SHARED + j->rc;
    switch (j->kind) {
    case TZ_JSON_NUMBER:
      if (j->flags & F_TEXT) {
        tz_publish_value(&tz_string_type, &j->u.number.text);
      }
      break;
    case TZ_JSON_STRING:
      tz_publish_value(&tz_string_type, &j->u.string);
      break;
    case TZ_JSON_ARRAY: {
      TzArray *a = j->u.array;
      if (a->rc > 0 && a->rc < TZ_RC_FOREVER) {
        a->rc = TZ_RC_SHARED + a->rc;
        TzJson **items = (TzJson **)a->data;
        for (uint32_t i = 0; i < a->length; ++i) {
          if (items[i]) {
            dead_push(&pending, items[i]);
          }
        }
      }
      break;
    }
    case TZ_JSON_OBJECT:
      for (uint32_t i = 0; i < j->u.object.count; ++i) {
        tz_publish_value(&tz_string_type, &j->u.object.entries[i].key);
        if (j->u.object.entries[i].value) {
          dead_push(&pending, j->u.object.entries[i].value);
        }
      }
      break;
    default:
      break;
    }
  }
  tz_free(pending.items);
}
#endif

void tz_json_free(TzJson *j) {
  Dead dead = {NULL, 0, 0};
  for (;;) {
    switch (j->kind) {
    case TZ_JSON_NUMBER:
      if (j->flags & F_TEXT) {
        tz_str_release_value(j->u.number.text);
      }
      break;
    case TZ_JSON_STRING:
      tz_str_release_value(j->u.string);
      break;
    case TZ_JSON_ARRAY: {
      // The elements are Json too, so they join the others here instead of
      // going through tz_arr_free.
      TzArray *a = j->u.array;
      if (tz_rc_dec(&a->rc)) {
        TzJson **items = (TzJson **)a->data;
        for (uint32_t i = 0; i < a->length; ++i) {
          drop_child(&dead, items[i]);
        }
        tz_free(a->alloc);
        tz_free(a);
        --tz_live_arrays;
      }
      break;
    }
    case TZ_JSON_OBJECT:
      for (uint32_t i = 0; i < j->u.object.count; ++i) {
        tz_str_release_value(j->u.object.entries[i].key);
        drop_child(&dead, j->u.object.entries[i].value);
      }
      tz_free(j->u.object.entries);
      tz_free(j->u.object.index);
      break;
    default: // null and booleans are never freed
      break;
    }
    tz_free(j);
    --tz_live_json;
    if (dead.count == 0) {
      break;
    }
    j = dead.items[--dead.count];
  }
  tz_free(dead.items);
}

// The release the compiler calls (CodeGen::releaseValue).
TZ_HOT void tz_json_release_value(TzJson *j) {
  if (j && tz_rc_dec(&j->rc)) {
    tz_json_free(j);
  }
}

//===----------------------------------------------------------------------===//
// Errors
//===----------------------------------------------------------------------===//

// Throws the error of `kind` (TZ_TYPE_ERROR, TZ_SYNTAX_ERROR) with the
// message in `text`, which node would throw; the caller returns next. JSON.parse,
// JSON.parse<T>, fromJson<T>, JSON.stringify and reading a key of null or
// undefined throw these in JavaScript.
static void json_throw(int32_t kind, TzBuf *text) {
  TzStr message = tz_buf_take(text);
  tz_throw_str(kind, message.obj, message.meta);
  tz_str_release_value(message);
}

//===----------------------------------------------------------------------===//
// Keys
//===----------------------------------------------------------------------===//

// The code units of a string. Must stay where it is while `units` is used:
// an inline string keeps them in `flat` itself.
typedef struct {
  TzStr flat;
  const void *units;
  uint32_t length;
  int two;
} View;

static void view_init(View *v, TzStr s) {
  v->flat = s;
  v->units = tz_str_units(&v->flat, &v->length, &v->two);
}

static uint16_t view_at(const View *v, uint32_t i) {
  return v->two ? ((const uint16_t *)v->units)[i] : ((const uint8_t *)v->units)[i];
}

static uint32_t key_hash(TzStr key) {
  View v;
  view_init(&v, key);
  uint32_t h = 2166136261u; // FNV-1a over the code units
  for (uint32_t i = 0; i < v.length; ++i) {
    h ^= view_at(&v, i);
    h *= 16777619u;
  }

  return h;
}

static int key_equal(TzStr a, TzStr b) { return tz_str_equal(a.obj, a.meta, b.obj, b.meta); }

// The value of a key that is an array index, "0" to "4294967294" without
// leading zeros; -1 for any other key.
static int64_t index_key(TzStr key) {
  View v;
  view_init(&v, key);
  if (v.length == 0 || v.length > 10 || (v.length > 1 && view_at(&v, 0) == '0')) {
    return -1;
  }
  int64_t n = 0;
  for (uint32_t i = 0; i < v.length; ++i) {
    uint16_t c = view_at(&v, i);
    if (c < '0' || c > '9') {
      return -1;
    }
    n = n * 10 + (c - '0');
  }

  return n <= INT64_C(4294967294) ? n : -1;
}

static int is_length_key(TzStr key) {
  TzStr length = tz_str_from_ascii("length", 6); // inline: nothing to release

  return key_equal(key, length);
}

static TzStr index_text(uint32_t i) {
  char text[16];
  int n = snprintf(text, sizeof text, "%u", i);

  return tz_str_from_ascii(text, (size_t)n);
}

//===----------------------------------------------------------------------===//
// Objects
//===----------------------------------------------------------------------===//

static void index_insert(uint32_t *index, uint32_t size, uint32_t hash, uint32_t position) {
  uint32_t at = hash & (size - 1);
  while (index[at]) {
    at = (at + 1) & (size - 1);
  }
  index[at] = position + 1;
}

static void index_build(TzJson *o) {
  uint32_t size = 16;
  while (size < o->u.object.count * 2 + 2) {
    size *= 2;
  }
  tz_free(o->u.object.index);
  uint32_t *index = (uint32_t *)tz_alloc(size * sizeof(uint32_t));
  memset(index, 0, size * sizeof(uint32_t));
  for (uint32_t i = 0; i < o->u.object.count; ++i) {
    index_insert(index, size, key_hash(o->u.object.entries[i].key), i);
  }
  o->u.object.index = index;
  o->u.object.index_size = size;
}

// The position of `key` among the entries of object `o`, or -1.
static int64_t object_find(const TzJson *o, TzStr key) {
  const TzJsonEntry *entries = o->u.object.entries;
  if (!o->u.object.index) {
    for (uint32_t i = 0; i < o->u.object.count; ++i) {
      if (key_equal(entries[i].key, key)) {
        return i;
      }
    }
    return -1;
  }
  uint32_t mask = o->u.object.index_size - 1;
  for (uint32_t at = key_hash(key) & mask;; at = (at + 1) & mask) {
    uint32_t found = o->u.object.index[at];
    if (!found) {
      return -1;
    }
    if (key_equal(entries[found - 1].key, key)) {
      return found - 1;
    }
  }
}

// Sets `key` (borrowed) to `value` (owned). A key that is there keeps its
// place, as in JavaScript.
static void object_put(TzJson *o, TzStr key, TzJson *value) {
  int64_t at = object_find(o, key);
  if (at >= 0) {
    TzJson *old = o->u.object.entries[at].value;
    o->u.object.entries[at].value = value;
    tz_json_release(old);
    return;
  }
  if (o->u.object.count == o->u.object.capacity) {
    uint32_t capacity = o->u.object.capacity * 2 + 4;
    TzJsonEntry *entries = (TzJsonEntry *)tz_alloc(capacity * sizeof(TzJsonEntry));
    if (o->u.object.count) {
      memcpy(entries, o->u.object.entries, o->u.object.count * sizeof(TzJsonEntry));
    }
    tz_free(o->u.object.entries);
    o->u.object.entries = entries;
    o->u.object.capacity = capacity;
  }
  // Keys are kept flat, so that comparing and hashing them reads them in place.
  uint32_t length;
  int two;
  tz_str_units(&key, &length, &two);
  tz_str_retain_value(key);
  uint32_t position = o->u.object.count++;
  o->u.object.entries[position].key = key;
  o->u.object.entries[position].value = value;
  if (index_key(key) >= 0) {
    o->flags |= F_INDEX_KEYS;
  }
  if (o->u.object.index && o->u.object.count * 2 + 2 <= o->u.object.index_size) {
    index_insert(o->u.object.index, o->u.object.index_size, key_hash(key), position);
  } else if (o->u.object.count > INDEX_FROM) {
    index_build(o);
  }
}

static void object_remove(TzJson *o, TzStr key) {
  int64_t at = object_find(o, key);
  if (at < 0) {
    return;
  }
  TzJsonEntry removed = o->u.object.entries[at];
  memmove(o->u.object.entries + at, o->u.object.entries + at + 1,
          (o->u.object.count - (uint32_t)at - 1) * sizeof(TzJsonEntry));
  --o->u.object.count;
  if (o->u.object.index) {
    index_build(o);
  }
  tz_str_release_value(removed.key);
  tz_json_release(removed.value);
}

typedef struct {
  int64_t index;
  uint32_t position;
} IndexKey;

static int compare_index_keys(const void *a, const void *b) {
  int64_t x = ((const IndexKey *)a)->index, y = ((const IndexKey *)b)->index;

  return x < y ? -1 : x > y;
}

uint32_t *tz_json_key_order(const TzJson *o) {
  if (!(o->flags & F_INDEX_KEYS)) {
    return NULL;
  }
  uint32_t count = o->u.object.count;
  uint32_t *order = (uint32_t *)tz_alloc((count + 1) * sizeof(uint32_t));
  IndexKey *indices = (IndexKey *)tz_alloc((count + 1) * sizeof(IndexKey));
  uint32_t n = 0, k = 0;
  for (uint32_t i = 0; i < count; ++i) {
    int64_t index = index_key(o->u.object.entries[i].key);
    if (index >= 0) {
      indices[n++] = (IndexKey){index, i};
    }
  }
  qsort(indices, n, sizeof(IndexKey), compare_index_keys);
  for (uint32_t i = 0; i < n; ++i) {
    order[k++] = indices[i].position;
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (index_key(o->u.object.entries[i].key) < 0) {
      order[k++] = i;
    }
  }
  tz_free(indices);

  return order;
}

//===----------------------------------------------------------------------===//
// Making values (CodeGen: literals and conversions)
//===----------------------------------------------------------------------===//

TzJson *tz_json_null(void) { return retained(&tz_json_null_node); }

TzJson *tz_json_bool(int32_t value) {
  return retained(value ? &tz_json_true_node : &tz_json_false_node);
}

TzJson *tz_json_number(double value) {
  TzJson *j = node_new(TZ_JSON_NUMBER, sizeof(double));
  j->u.number.value = value;

  return j;
}

static TzJson *string_node(TzStr s) { // takes over s
  TzJson *j = node_new(TZ_JSON_STRING, sizeof(TzStr));
  j->u.string = s;

  return j;
}

// The string is borrowed.
TzJson *tz_json_string(void *obj, uint64_t meta) {
  TzStr s = {obj, meta};
  tz_str_retain_value(s);

  return string_node(s);
}

// Takes over the reference to `array`, an array of Json.
TzJson *tz_json_from_array(TzArray *array) {
  TzJson *j = node_new(TZ_JSON_ARRAY, sizeof(TzArray *));
  j->u.array = array;

  return j;
}

static TzJson *array_node(uint32_t capacity) {
  return tz_json_from_array(tz_arr_new(&tz_json_type, 0, capacity));
}

TzJson *tz_json_object_new(void) {
  TzJson *j = node_new(TZ_JSON_OBJECT, sizeof(j->u.object));
  j->u.object.entries = NULL;
  j->u.object.count = j->u.object.capacity = 0;
  j->u.object.index = NULL;
  j->u.object.index_size = 0;

  return j;
}

// `{ key: value }` of a literal: the key is borrowed, the value taken over.
void tz_json_object_put(TzJson *o, void *key_obj, uint64_t key_meta, TzJson *value) {
  TzStr key = {key_obj, key_meta};
  object_put(o, key, value);
}

//===----------------------------------------------------------------------===//
// Reading values
//===----------------------------------------------------------------------===//

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
// (Sema narrowed it with typeof or Array.isArray). The string and the array
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
    return key_equal(a->u.string, b->u.string);
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

// What JavaScript calls the value in messages: "undefined", "null"...
static const char *kind_name(const TzJson *j) {
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
  tz_buf_ascii(&b, kind_name(j));
  tz_buf_ascii(&b, " (");
  tz_buf_ascii(&b, verb[0] == 'r' ? "reading '" : "setting '");
  tz_buf_str(&b, key);
  tz_buf_ascii(&b, "')");
  json_throw(TZ_TYPE_ERROR, &b);
}

// The element `i` of an array, or the code unit `i` of a string, as a Json;
// NULL past the end.
static TzJson *element_of(const TzJson *j, uint32_t i) {
  if (j->kind == TZ_JSON_ARRAY) {
    const TzArray *a = j->u.array;
    return i < a->length ? retained(((TzJson **)a->data)[i]) : NULL;
  }
  TzStr s = j->u.string;
  View v;
  view_init(&v, s);
  if (i >= v.length) {
    return NULL;
  }
  uint16_t unit = view_at(&v, i);

  return string_node(tz_str_from_utf16(&unit, 1));
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
    int64_t at = object_find(j, key);
    return at >= 0 ? retained(j->u.object.entries[at].value) : NULL;
  }
  case TZ_JSON_ARRAY:
  case TZ_JSON_STRING: {
    int64_t index = index_key(key);
    if (index >= 0) {
      return element_of(j, (uint32_t)index);
    }
    if (!is_length_key(key)) {
      return NULL;
    }
    if (j->kind == TZ_JSON_ARRAY) {
      return tz_json_number(j->u.array->length);
    }
    View v;
    view_init(&v, j->u.string);
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

//===----------------------------------------------------------------------===//
// Writing values
//===----------------------------------------------------------------------===//

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
    json_throw(TZ_TYPE_ERROR, &b);
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
      object_put((TzJson *)j, key, value);
    } else {
      object_remove((TzJson *)j, key);
    }
    return;
  }
  int64_t index = j && j->kind == TZ_JSON_ARRAY ? index_key(key) : -1;
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
    tz_buf_ascii(&b, kind_name(j));
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
  json_throw(TZ_TYPE_ERROR, &b);
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
// JSON.parse
//===----------------------------------------------------------------------===//

// An iterative parser with the grammar, the errors and the messages of V8's
// JsonParser, so that a bad text fails as it does in node. Positions count
// UTF-16 code units.

enum {
  T_STRING,
  T_NUMBER,
  T_LBRACE,
  T_RBRACE,
  T_LBRACK,
  T_RBRACK,
  T_TRUE,
  T_FALSE,
  T_NULL,
  T_COLON,
  T_COMMA,
  T_WHITESPACE,
  T_ILLEGAL,
  T_EOS,
};

// An object or an array that is being read, and for an object the key whose
// value comes next.
typedef struct {
  TzJson *container;
  TzStr key;
  int has_key;
} Frame;

typedef struct {
  TzBuf text; // the code units of the whole text
  const uint16_t *s;
  uint32_t n, pos;
  Frame *frames;
  size_t depth, capacity;
  // What an error must release besides the frames: a value that waits to be
  // stored, the text of a string being read.
  TzJson *pending;
  TzBuf scratch;
  // Where an error goes back to (__builtin_setjmp): tz_json_parse.
  void *fail[5];
} Parser;

static int32_t current(const Parser *p) { return p->pos < p->n ? p->s[p->pos] : -1; }

static int is_digit(int32_t c) { return c >= '0' && c <= '9'; }

static int token_of(int32_t c) {
  if (c < 0) {
    return T_EOS;
  }
  if (is_digit(c) || c == '-') {
    return T_NUMBER;
  }
  switch (c) {
  case '"':
    return T_STRING;
  case '{':
    return T_LBRACE;
  case '}':
    return T_RBRACE;
  case '[':
    return T_LBRACK;
  case ']':
    return T_RBRACK;
  case 't':
    return T_TRUE;
  case 'f':
    return T_FALSE;
  case 'n':
    return T_NULL;
  case ':':
    return T_COLON;
  case ',':
    return T_COMMA;
  case ' ':
  case '\t':
  case '\n':
  case '\r':
    return T_WHITESPACE;
  default:
    return T_ILLEGAL;
  }
}

static void skip_whitespace(Parser *p) {
  while (p->pos < p->n && token_of(p->s[p->pos]) == T_WHITESPACE) {
    ++p->pos;
  }
}

static void parser_release(Parser *p) {
  for (size_t i = 0; i < p->depth; ++i) {
    if (p->frames[i].has_key) {
      tz_str_release_value(p->frames[i].key);
    }
    tz_json_release(p->frames[i].container);
  }
  tz_free(p->frames);
  p->frames = NULL;
  p->depth = 0;
  tz_json_release(p->pending);
  p->pending = NULL;
  tz_buf_free(&p->scratch);
}

// " in JSON at position 5 (line 1 column 6)": V8 counts lines at \n, \r and
// \r\n, the line terminators of JSON.
static void at_position(TzBuf *b, const Parser *p) {
  uint32_t line = 1, line_start = 0;
  for (uint32_t i = 0; i < p->pos; ++i) {
    if (p->s[i] == '\r' && i + 1 < p->pos && p->s[i + 1] == '\n') {
      ++i;
    }
    if (p->s[i] == '\r' || p->s[i] == '\n') {
      ++line;
      line_start = i + 1;
    }
  }
  char text[96];
  int n = snprintf(text, sizeof text, " at position %u (line %u column %u)", p->pos, line,
                   p->pos - line_start + 1);
  tz_buf_ascii_n(b, text, (size_t)n);
}

static int is_special(const Parser *p) {
  static const char *const words[] = {"[object Object]", "undefined", "Infinity", "NaN"};
  for (size_t w = 0; w < sizeof words / sizeof *words; ++w) {
    size_t length = strlen(words[w]);
    if (length != p->n) {
      continue;
    }
    size_t i = 0;
    while (i < length && p->s[i] == (uint16_t)words[w][i]) {
      ++i;
    }
    if (i == length) {
      return 1;
    }
  }

  return 0;
}

// V8's ReportUnexpectedToken: the token at the current position does not
// fit. `message` replaces the general one about the token, with the
// position; without it, a token that is not a number or a string is shown
// with the text around it.
TZ_NORETURN static void unexpected(Parser *p, int token, const char *message) {
  parser_release(p);
  TzBuf b;
  tz_buf_init(&b);
  if (message) {
    tz_buf_ascii(&b, message);
    at_position(&b, p);
  } else if (token == T_EOS) {
    tz_buf_ascii(&b, "Unexpected end of JSON input");
  } else if (token == T_NUMBER || token == T_STRING) {
    tz_buf_ascii(&b, token == T_NUMBER ? "Unexpected number in JSON" : "Unexpected string in JSON");
    at_position(&b, p);
  } else if (is_special(p)) {
    tz_buf_push(&b, '"');
    tz_buf_units(&b, p->s, p->n);
    tz_buf_ascii(&b, "\" is not valid JSON");
  } else {
    tz_buf_ascii(&b, "Unexpected token '");
    tz_buf_push(&b, p->s[p->pos]);
    tz_buf_ascii(&b, "', ");
    // Ten code units on each side of a long text.
    uint32_t from = 0, to = p->n;
    const char *before = "", *after = "";
    if (p->n >= 21) {
      if (p->pos < 10) {
        to = p->pos + 10;
        after = "...";
      } else if (p->pos < p->n - 10) {
        from = p->pos - 10;
        to = p->pos + 10;
        before = after = "...";
      } else {
        from = p->pos - 10;
        before = "...";
      }
    }
    tz_buf_ascii(&b, before);
    tz_buf_push(&b, '"');
    tz_buf_units(&b, p->s + from, to - from);
    tz_buf_push(&b, '"');
    tz_buf_ascii(&b, after);
    tz_buf_ascii(&b, " is not valid JSON");
  }
  tz_buf_free(&p->text);
  json_throw(TZ_SYNTAX_ERROR, &b);
  __builtin_longjmp(p->fail, 1);
}

TZ_NORETURN static void unexpected_here(Parser *p) { unexpected(p, token_of(current(p)), NULL); }

static void scan_literal(Parser *p, const char *word) {
  ++p->pos; // the first letter chose the literal
  for (size_t i = 1; word[i]; ++i, ++p->pos) {
    if (p->pos >= p->n) {
      unexpected(p, T_EOS, NULL);
    }
    if (p->s[p->pos] != (uint16_t)word[i]) {
      unexpected_here(p);
    }
  }
}

static int hex_value(int32_t c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }

  return -1;
}

// A string, after its opening quote; returns an owned string.
static TzStr scan_string(Parser *p) {
  uint32_t start = p->pos;
  for (;;) { // the usual string, without escapes, is taken as it is
    if (p->pos >= p->n) {
      unexpected(p, T_ILLEGAL, "Unterminated string in JSON");
    }
    uint16_t c = p->s[p->pos];
    if (c == '"') {
      TzStr s = tz_str_from_utf16(p->s + start, p->pos - start);
      ++p->pos;
      return s;
    }
    if (c == '\\') {
      break;
    }
    if (c < 0x20) {
      unexpected(p, T_ILLEGAL, "Bad control character in string literal in JSON");
    }
    ++p->pos;
  }
  TzBuf *b = &p->scratch;
  b->length = 0;
  tz_buf_units(b, p->s + start, p->pos - start);
  for (;;) {
    if (p->pos >= p->n) {
      unexpected(p, T_ILLEGAL, "Unterminated string in JSON");
    }
    uint16_t c = p->s[p->pos];
    if (c == '"') {
      ++p->pos;
      return tz_str_from_utf16(b->data, b->length);
    }
    if (c < 0x20) {
      unexpected(p, T_ILLEGAL, "Bad control character in string literal in JSON");
    }
    if (c != '\\') {
      tz_buf_push(b, c);
      ++p->pos;
      continue;
    }
    if (++p->pos >= p->n) {
      unexpected(p, T_EOS, NULL);
    }
    c = p->s[p->pos];
    switch (c) {
    case '"':
    case '\\':
    case '/':
      tz_buf_push(b, c);
      break;
    case 'b':
      tz_buf_push(b, '\b');
      break;
    case 'f':
      tz_buf_push(b, '\f');
      break;
    case 'n':
      tz_buf_push(b, '\n');
      break;
    case 'r':
      tz_buf_push(b, '\r');
      break;
    case 't':
      tz_buf_push(b, '\t');
      break;
    case 'u': {
      uint32_t value = 0;
      for (int k = 0; k < 4; ++k) {
        ++p->pos;
        int digit = hex_value(current(p));
        if (digit < 0) {
          unexpected(p, T_ILLEGAL, "Bad Unicode escape in JSON");
        }
        value = value * 16 + (uint32_t)digit;
      }
      tz_buf_push(b, (uint16_t)value);
      break;
    }
    default:
      unexpected(p, T_ILLEGAL, "Bad escaped character in JSON");
    }
    ++p->pos;
  }
}

static TzJson *parse_number(Parser *p) {
  uint32_t start = p->pos;
  if (current(p) == '-') {
    ++p->pos;
  }
  if (current(p) == '0') {
    // A zero is the whole integer part: "01" is not a number.
    ++p->pos;
    if (is_digit(current(p))) {
      unexpected(p, T_NUMBER, NULL);
    }
  } else {
    if (!is_digit(current(p))) {
      unexpected(p, T_ILLEGAL, "No number after minus sign in JSON");
    }
    while (is_digit(current(p))) {
      ++p->pos;
    }
  }
  if (current(p) == '.') {
    ++p->pos;
    if (!is_digit(current(p))) {
      unexpected(p, T_ILLEGAL, "Unterminated fractional number in JSON");
    }
    while (is_digit(current(p))) {
      ++p->pos;
    }
  }
  if (current(p) == 'e' || current(p) == 'E') {
    ++p->pos;
    if (current(p) == '-' || current(p) == '+') {
      ++p->pos;
    }
    if (!is_digit(current(p))) {
      unexpected(p, T_ILLEGAL, "Exponent part is missing a number in JSON");
    }
    while (is_digit(current(p))) {
      ++p->pos;
    }
  }
  uint32_t length = p->pos - start;
  char local[64];
  char *text = length < sizeof local ? local : (char *)tz_alloc(length + 1);
  uint32_t digits = 0, nonzero = 0;
  int exponent = 0;
  for (uint32_t i = 0; i < length; ++i) {
    char c = (char)p->s[start + i];
    text[i] = c;
    exponent |= c == 'e' || c == 'E';
    if (!exponent && is_digit(c)) {
      ++digits;
      nonzero |= c != '0';
    }
  }
  text[length] = 0;
  double value = strtod(text, NULL); // correctly rounded, as JavaScript requires
  // The text stays where the double cannot give its digits back: more than
  // 15 of them, or beyond the range of a double. JSON.parse<T> reads a
  // decimal or a long from it exactly.
  int keep = digits > 15 || isinf(value) || (value == 0 && nonzero);
  TzJson *j;
  if (keep) {
    j = node_new(TZ_JSON_NUMBER, sizeof(j->u.number));
    j->flags |= F_TEXT;
    j->u.number.text = tz_str_from_ascii(text, length);
  } else {
    j = node_new(TZ_JSON_NUMBER, sizeof(double));
  }
  j->u.number.value = value;
  if (text != local) {
    tz_free(text);
  }

  return j;
}

static void push_frame(Parser *p, TzJson *container) {
  if (p->depth == p->capacity) {
    size_t capacity = p->capacity * 2 + 16;
    Frame *frames = (Frame *)tz_alloc(capacity * sizeof(Frame));
    if (p->depth) {
      memcpy(frames, p->frames, p->depth * sizeof(Frame));
    }
    tz_free(p->frames);
    p->frames = frames;
    p->capacity = capacity;
  }
  Frame *f = &p->frames[p->depth++];
  f->container = container;
  f->has_key = 0;
}

// A key and its colon, in the object of the innermost frame.
static void read_key(Parser *p, const char *message) {
  skip_whitespace(p);
  if (current(p) != '"') {
    unexpected(p, token_of(current(p)), message);
  }
  ++p->pos;
  Frame *f = &p->frames[p->depth - 1];
  f->key = scan_string(p);
  f->has_key = 1;
  skip_whitespace(p);
  if (current(p) != ':') {
    unexpected(p, token_of(current(p)), "Expected ':' after property name in JSON");
  }
  ++p->pos;
}

static TzJson *parse(Parser *p) {
  for (;;) {
    TzJson *value;
    // A value, or the start of an array or an object.
    skip_whitespace(p);
    switch (token_of(current(p))) {
    case T_STRING:
      ++p->pos;
      value = string_node(scan_string(p));
      break;
    case T_NUMBER:
      value = parse_number(p);
      break;
    case T_LBRACE:
      ++p->pos;
      skip_whitespace(p);
      if (current(p) == '}') {
        ++p->pos;
        value = tz_json_object_new();
        break;
      }
      push_frame(p, tz_json_object_new());
      read_key(p, "Expected property name or '}' in JSON");
      continue;
    case T_LBRACK:
      ++p->pos;
      skip_whitespace(p);
      if (current(p) == ']') {
        ++p->pos;
        value = array_node(0);
        break;
      }
      push_frame(p, array_node(4));
      continue;
    case T_TRUE:
      scan_literal(p, "true");
      value = retained(&tz_json_true_node);
      break;
    case T_FALSE:
      scan_literal(p, "false");
      value = retained(&tz_json_false_node);
      break;
    case T_NULL:
      scan_literal(p, "null");
      value = retained(&tz_json_null_node);
      break;
    default:
      unexpected_here(p);
    }

    // The value goes into the containers it completes.
    for (;;) {
      if (p->depth == 0) {
        skip_whitespace(p);
        if (p->pos < p->n) {
          p->pending = value;
          unexpected(p, token_of(current(p)), "Unexpected non-whitespace character after JSON");
        }
        return value;
      }
      Frame *f = &p->frames[p->depth - 1];
      TzJson *container = f->container;
      if (container->kind == TZ_JSON_OBJECT) {
        object_put(container, f->key, value);
        tz_str_release_value(f->key);
        f->has_key = 0;
        skip_whitespace(p);
        if (current(p) == ',') {
          ++p->pos;
          read_key(p, "Expected double-quoted property name in JSON");
          break;
        }
        --p->depth;
        if (current(p) != '}') {
          p->pending = container;
          unexpected(p, token_of(current(p)), "Expected ',' or '}' after property value in JSON");
        }
      } else {
        tz_arr_push_value(container->u.array, &value);
        skip_whitespace(p);
        if (current(p) == ',') {
          ++p->pos;
          break;
        }
        --p->depth;
        if (current(p) != ']') {
          p->pending = container;
          unexpected(p, token_of(current(p)), "Expected ',' or ']' after array element in JSON");
        }
      }
      ++p->pos;
      value = container;
    }
  }
}

// JSON.parse(text). A text that is not JSON is a SyntaxError, and NULL:
// unexpected released what the parser had and came back here.
TzJson *tz_json_parse(void *obj, uint64_t meta) {
  TzStr str = {obj, meta};
  Parser p;
  memset(&p, 0, sizeof p);
  if (__builtin_setjmp(p.fail)) {
    return NULL;
  }
  tz_buf_init(&p.text);
  tz_buf_str(&p.text, str);
  p.s = p.text.data;
  p.n = (uint32_t)p.text.length;
  tz_buf_init(&p.scratch);
  TzJson *result = parse(&p);
  tz_free(p.frames);
  tz_buf_free(&p.scratch);
  tz_buf_free(&p.text);

  return result;
}

// The values of embedJson, kept for the leak check: the address of each
// cache the program filled.
typedef struct {
  TzJson **slots[64];
  TzJson ***more;
  size_t count, capacity;
} Caches;
Caches tz_json_caches; // not static: see the rules in tzrt.h

// embedJson(path): the text of the file, checked when the program was
// compiled, is read the first time the program gets there; `cache` keeps
// the value for the next times, so the same file gives the same value, as
// require() of a JSON file in node does.
// Threads fill a cache once, under the lock, with a value they share
// (publish.c).
TzLock tz_json_embed_lock;

TzJson *tz_json_embed(TzJson **cache, void *obj, uint64_t meta) {
  TzJson *found = __atomic_load_n(cache, __ATOMIC_ACQUIRE);
  if (found) {
    return retained(found);
  }
  tz_lock(&tz_json_embed_lock);
  if (!*cache) {
    TzJson *parsed = tz_json_parse(obj, meta);
#if TZ_PARALLEL
    tz_json_publish(parsed);
#endif
    __atomic_store_n(cache, parsed, __ATOMIC_RELEASE);
    Caches *c = &tz_json_caches;
    if (c->count < 64) {
      c->slots[c->count] = cache;
    } else {
      size_t extra = c->count - 64;
      if (extra == c->capacity) {
        size_t capacity = c->capacity * 2 + 16;
        TzJson ***more = (TzJson ***)tz_alloc(capacity * sizeof(TzJson **));
        if (extra) {
          memcpy(more, c->more, extra * sizeof(TzJson **));
        }
        tz_free(c->more);
        c->more = more;
        c->capacity = capacity;
      }
      c->more[extra] = cache;
    }
    ++c->count;
  }
  found = *cache;
  tz_unlock(&tz_json_embed_lock);

  return retained(found);
}

void tz_json_shutdown(void) {
  Caches *c = &tz_json_caches;
  for (size_t i = 0; i < c->count; ++i) {
    TzJson **cache = i < 64 ? c->slots[i] : c->more[i - 64];
    TzJson *j = *cache;
    *cache = NULL;
    tz_json_release(j);
  }
  tz_free(c->more);
  memset(c, 0, sizeof *c);
}

//===----------------------------------------------------------------------===//
// JSON.stringify
//===----------------------------------------------------------------------===//

typedef struct {
  TzBuf out;
  TzBuf gap;    // what each level indents by; empty for none
  TzBuf indent; // the current indentation
  // The arrays and objects being written, to find one inside itself.
  const void **open;
  size_t depth, capacity;
  // Where an error goes back to (__builtin_setjmp): tz_json_stringify.
  void *fail[5];
} Writer;

static void writer_release(Writer *w) {
  tz_buf_free(&w->out);
  tz_buf_free(&w->gap);
  tz_buf_free(&w->indent);
  tz_free((void *)w->open);
}

static void enter(Writer *w, const void *container) {
  for (size_t i = 0; i < w->depth; ++i) {
    if (w->open[i] == container) {
      writer_release(w);
      TzBuf b;
      tz_buf_init(&b);
      tz_buf_ascii(&b, "Converting circular structure to JSON");
      json_throw(TZ_TYPE_ERROR, &b);
      __builtin_longjmp(w->fail, 1);
    }
  }
  if (w->depth == w->capacity) {
    size_t capacity = w->capacity * 2 + 16;
    const void **open = (const void **)tz_alloc(capacity * sizeof(void *));
    if (w->depth) {
      memcpy((void *)open, w->open, w->depth * sizeof(void *));
    }
    tz_free((void *)w->open);
    w->open = open;
    w->capacity = capacity;
  }
  w->open[w->depth++] = container;
  tz_buf_units(&w->indent, w->gap.data, w->gap.length);
}

static void leave(Writer *w) {
  --w->depth;
  w->indent.length -= w->gap.length;
}

// Before an element or a property: the comma, and with a gap a new line.
static void separate(Writer *w, uint32_t written) {
  if (written) {
    tz_buf_push(&w->out, ',');
  }
  if (w->gap.length) {
    tz_buf_push(&w->out, '\n');
    tz_buf_units(&w->out, w->indent.data, w->indent.length);
  }
}

// After the last one, and after leave(), before the bracket: the new line and
// the outer indentation, when something was written.
static void close_with(Writer *w, uint32_t written, char bracket) {
  if (written && w->gap.length) {
    tz_buf_push(&w->out, '\n');
    tz_buf_units(&w->out, w->indent.data, w->indent.length);
  }
  tz_buf_push(&w->out, (uint16_t)bracket);
}

static const char hex_digits[] = "0123456789abcdef";

// QuoteJSONString: the escapes of JSON, \u with lowercase hex for the other
// control characters and for lone surrogates.
static void quote(TzBuf *out, TzStr s) {
  View v;
  view_init(&v, s);
  tz_buf_push(out, '"');
  for (uint32_t i = 0; i < v.length; ++i) {
    uint16_t c = view_at(&v, i);
    switch (c) {
    case '"':
      tz_buf_ascii(out, "\\\"");
      continue;
    case '\\':
      tz_buf_ascii(out, "\\\\");
      continue;
    case '\b':
      tz_buf_ascii(out, "\\b");
      continue;
    case '\f':
      tz_buf_ascii(out, "\\f");
      continue;
    case '\n':
      tz_buf_ascii(out, "\\n");
      continue;
    case '\r':
      tz_buf_ascii(out, "\\r");
      continue;
    case '\t':
      tz_buf_ascii(out, "\\t");
      continue;
    default:
      break;
    }
    int lone = 0;
    if (c >= 0xD800 && c <= 0xDBFF) {
      if (i + 1 < v.length && view_at(&v, i + 1) >= 0xDC00 && view_at(&v, i + 1) <= 0xDFFF) {
        tz_buf_push(out, c);
        tz_buf_push(out, view_at(&v, ++i));
        continue;
      }
      lone = 1;
    } else if (c >= 0xDC00 && c <= 0xDFFF) {
      lone = 1;
    }
    if (c < 0x20 || lone) {
      char text[7] = {'\\',
                      'u',
                      hex_digits[c >> 12],
                      hex_digits[(c >> 8) & 15],
                      hex_digits[(c >> 4) & 15],
                      hex_digits[c & 15],
                      0};
      tz_buf_ascii_n(out, text, 6);
    } else {
      tz_buf_push(out, c);
    }
  }
  tz_buf_push(out, '"');
}

static void write_number(TzBuf *out, double value) {
  if (!isfinite(value)) {
    tz_buf_ascii(out, "null");
    return;
  }
  char text[32];
  tz_buf_ascii_n(out, text, tz_format_f64(value == 0 ? 0 : value, text)); // -0 is "0"
}

static int write_value(Writer *w, const TzType *type, const void *value);

static int write_json(Writer *w, const TzJson *j) {
  if (!j) {
    return 0;
  }
  switch (j->kind) {
  case TZ_JSON_NULL:
    tz_buf_ascii(&w->out, "null");
    return 1;
  case TZ_JSON_BOOL:
    tz_buf_ascii(&w->out, j->u.boolean ? "true" : "false");
    return 1;
  case TZ_JSON_NUMBER:
    write_number(&w->out, j->u.number.value);
    return 1;
  case TZ_JSON_STRING:
    quote(&w->out, j->u.string);
    return 1;
  case TZ_JSON_ARRAY:
    return write_value(w, NULL, &j->u.array);
  default:
    break;
  }
  enter(w, j);
  tz_buf_push(&w->out, '{');
  uint32_t *order = tz_json_key_order(j);
  uint32_t written = 0;
  for (uint32_t k = 0; k < j->u.object.count; ++k) {
    const TzJsonEntry *e = &j->u.object.entries[order ? order[k] : k];
    separate(w, written++);
    quote(&w->out, e->key);
    tz_buf_ascii(&w->out, w->gap.length ? ": " : ":");
    write_json(w, e->value);
  }
  tz_free(order);
  leave(w);
  close_with(w, written, '}');

  return 1;
}

// Whether a value would be written at all: undefined and functions are not.
static int has_json(const TzType *type, const void *value) {
  type = tz_resolve(type, &value);
  switch (type->kind) {
  case TZ_T_UNDEFINED:
  case TZ_T_FUNCTION:
    return 0;
  case TZ_T_OPTIONAL:
    return tz_optional_has(type, value) && has_json(type->inner, value);
  case TZ_T_JSON:
    return *(TzJson *const *)value != NULL;
  case TZ_T_UNKNOWN: {
    // What it holds: a box's value, or an object.
    const TzInstance *u = *(const TzInstance *const *)value;
    if (!u) {
      return 0;
    }
    if (u->layout->flags & TZ_LAYOUT_BOX) {
      return has_json(((const TzBox *)u)->type, TZ_BOX_VALUE(u));
    }
    return 1;
  }
  default:
    return 1;
  }
}

// An array of values of `type`, laid out one after another; `type` NULL for
// an array of Json.
static void write_elements(Writer *w, const TzType *type, const char *data, uint32_t count,
                           uint32_t stride) {
  for (uint32_t i = 0; i < count; ++i) {
    separate(w, i);
    const void *value = data + (size_t)i * stride;
    if (!(type ? write_value(w, type, value) : write_json(w, *(TzJson *const *)value))) {
      tz_buf_ascii(&w->out, "null");
    }
  }
}

// A key of a field, which a descriptor holds as UTF-8, quoted.
static void quote_utf8(TzBuf *out, const char *name, size_t n) {
  TzBuf key;
  tz_buf_init(&key);
  for (size_t i = 0; i < n;) {
    uint32_t c = (unsigned char)name[i];
    size_t extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (extra) {
      c &= 0x3Fu >> extra;
    }
    for (size_t k = 1; k <= extra && i + k < n; ++k) {
      c = (c << 6) | ((unsigned char)name[i + k] & 0x3F);
    }
    i += extra + 1;
    if (c >= 0x10000) {
      c -= 0x10000;
      tz_buf_push(&key, (uint16_t)(0xD800 + (c >> 10)));
      tz_buf_push(&key, (uint16_t)(0xDC00 + (c & 0x3FF)));
    } else {
      tz_buf_push(&key, (uint16_t)c);
    }
  }
  TzStr s = tz_buf_take(&key);
  quote(out, s);
  tz_str_release_value(s);
}

// Writes a value of `type` (NULL: an array of Json, at `value`) as
// JSON.stringify does; returns 0 for a value it leaves out (undefined, a
// function), and then writes nothing.
static int write_value(Writer *w, const TzType *type, const void *value) {
  TzBuf *out = &w->out;
  if (!type) {
    const TzArray *a = *(TzArray *const *)value;
    enter(w, a);
    tz_buf_push(out, '[');
    write_elements(w, NULL, a->data, a->length, sizeof(TzJson *));
    leave(w);
    close_with(w, a->length, ']');
    return 1;
  }
  type = tz_resolve(type, &value);
  switch (type->kind) {
  case TZ_T_INT:
    write_number(out, *(const int32_t *)value);
    return 1;
  case TZ_T_LONG:
    // Up to 2^53 a double holds it; JavaScript would print beyond that what
    // its double holds, and so does this.
    write_number(out, (double)*(const int64_t *)value);
    return 1;
  case TZ_T_DOUBLE:
    write_number(out, *(const double *)value);
    return 1;
  case TZ_T_BOOL:
    tz_buf_ascii(out, *(const uint8_t *)value ? "true" : "false");
    return 1;
  case TZ_T_STRING:
    quote(out, *(const TzStr *)value);
    return 1;
  case TZ_T_DECIMAL: {
    // A string, as BigNumber.js writes it (docs/decimal.md): as a number
    // JavaScript would read it back inexactly.
    TzBuf text;
    tz_buf_init(&text);
    tz_buf_decimal(&text, *(const TzDec *)value);
    TzStr s = tz_buf_take(&text);
    quote(out, s);
    tz_str_release_value(s);
    return 1;
  }
  case TZ_T_JSON:
    return write_json(w, *(TzJson *const *)value);
  case TZ_T_UNKNOWN: {
    static const TzType object_type = {TZ_T_OBJECT, sizeof(void *), NULL};
    const TzInstance *u = *(const TzInstance *const *)value;
    if (!u) {
      return 0;
    }
    if (u->layout->flags & TZ_LAYOUT_BOX) {
      return write_value(w, ((const TzBox *)u)->type, TZ_BOX_VALUE(u));
    }
    return write_value(w, &object_type, value);
  }
  case TZ_T_OPTIONAL:
    return tz_optional_has(type, value) && write_value(w, type->inner, value);
  case TZ_T_ARRAY: {
    const TzArray *a = *(TzArray *const *)value;
    enter(w, a);
    tz_buf_push(out, '[');
    write_elements(w, a->type, a->data, a->length, a->type->size);
    leave(w);
    close_with(w, a->length, ']');
    return 1;
  }
  case TZ_T_RESULT: {
    // The objects of neverthrow's classes Ok and Err have one property.
    const TzResultType *result = (const TzResultType *)type;
    const char *object = (const char *)value;
    int ok = object[result->ok_offset] != 0;
    const TzType *half = ok ? type->inner : result->error;
    const void *held = object + (ok ? result->value_offset : result->error_offset);
    enter(w, value);
    tz_buf_push(out, '{');
    uint32_t written = 0;
    if (has_json(half, held)) {
      separate(w, written++);
      tz_buf_ascii(out, ok ? "\"value\"" : "\"error\"");
      tz_buf_ascii(out, w->gap.length ? ": " : ":");
      write_value(w, half, held);
    }
    leave(w);
    close_with(w, written, '}');
    return 1;
  }
  case TZ_T_OBJECT: {
    const TzInstance *object = *(TzInstance *const *)value;
    const TzLayout *layout = object->layout;
    const char *fields = (const char *)object;
    // An unknown's box, seen as an object: what it holds. A class is a
    // function in JavaScript, which JSON leaves out.
    if (layout->flags & TZ_LAYOUT_BOX) {
      return write_value(w, ((const TzBox *)object)->type, TZ_BOX_VALUE(object));
    }
    if (layout->flags & TZ_LAYOUT_CLASS) {
      return 0;
    }
    // The match of a regex is the array of its captures in JavaScript; its
    // index, input, groups and indices are no elements of it.
    if (layout->flags & TZ_LAYOUT_MATCH) {
      return write_value(w, layout->fields[0].type, fields + layout->fields[0].offset);
    }
    // A date is its toJSON(): the ISO text, or null for an invalid date.
    if (layout->flags & TZ_LAYOUT_DATE) {
      TzStr text;
      if (tz_date_json(&text, ((const TzDate *)object)->time)) {
        quote(out, text);
        tz_str_release_value(text);
      } else {
        tz_buf_ascii(out, "null");
      }
      return 1;
    }
    // A typed array has its indices as keys, `{"0":1,"1":2}`; a Buffer is
    // its toJSON(), `{"type":"Buffer","data":[1,2]}`; the other objects of
    // binary data have no keys.
    if ((layout->flags & TZ_LAYOUT_BYTES) && tz_bytes_kind(object) <= TZ_BYTES_BUFFER) {
      const TzBytes *bytes = (const TzBytes *)object;
      const char *colon = w->gap.length ? ": " : ":";
      enter(w, object);
      tz_buf_push(out, '{');
      int buffer = tz_bytes_kind(object) == TZ_BYTES_BUFFER;
      if (buffer) {
        separate(w, 0);
        tz_buf_ascii(out, "\"type\"");
        tz_buf_ascii(out, colon);
        tz_buf_ascii(out, "\"Buffer\"");
        separate(w, 1);
        tz_buf_ascii(out, "\"data\"");
        tz_buf_ascii(out, colon);
        enter(w, &bytes->data);
        tz_buf_push(out, '[');
      }
      for (int32_t i = 0; i < bytes->length; ++i) {
        separate(w, (uint32_t)i);
        if (!buffer) {
          char key[16];
          int n = snprintf(key, sizeof key, "\"%d\"", i);
          tz_buf_ascii_n(out, key, (size_t)n);
          tz_buf_ascii(out, colon);
        }
        write_number(out, tz_bytes_get(bytes, i));
      }
      if (buffer) {
        leave(w);
        close_with(w, (uint32_t)bytes->length, ']');
      }
      leave(w);
      close_with(w, buffer ? 2 : (uint32_t)bytes->length, '}');
      return 1;
    }
    enter(w, object);
    // A Record is an object of its keys; a Map and a Set have no own
    // properties in JavaScript: `{}`.
    if (layout->flags & TZ_LAYOUT_DICT) {
      const TzMap *m = (const TzMap *)object;
      const TzMapLayout *l = m->layout;
      tz_buf_push(out, '{');
      uint32_t written = 0, at = 0;
      if (l->kind == TZ_MAP_RECORD) {
        for (const char *e; (e = (const char *)tz_map_next(m, &at));) {
          if (!has_json(l->value, e + l->value_offset)) {
            continue;
          }
          separate(w, written++);
          quote(out, *(const TzStr *)(e + l->key_offset));
          tz_buf_ascii(out, w->gap.length ? ": " : ":");
          write_value(w, l->value, e + l->value_offset);
        }
      }
      leave(w);
      close_with(w, written, '}');
      return 1;
    }
    if (layout->flags & TZ_LAYOUT_TUPLE) {
      tz_buf_push(out, '[');
      for (uint32_t i = 0; i < layout->count; ++i) {
        separate(w, i);
        if (!write_value(w, layout->fields[i].type, fields + layout->fields[i].offset)) {
          tz_buf_ascii(out, "null");
        }
      }
      leave(w);
      close_with(w, layout->count, ']');
      return 1;
    }
    // The fields in the order the object prints them; methods are
    // functions, and an iterator has no fields of its own: `{}`. The
    // message and the cause of an Error are no keys in JavaScript, nor is
    // its name, unless the program gave it one (`this.name = ...`), which
    // usually comes after the fields of its class.
    int error = (layout->flags & TZ_LAYOUT_ERROR) != 0;
    const TzField *name = NULL;
    tz_buf_push(out, '{');
    uint32_t written = 0;
    for (uint32_t i = 0; i < layout->count; ++i) {
      const TzField *field = &layout->fields[i];
      if (error && tz_error_base_field(field)) {
        if (field->name_length == 4 && memcmp(field->name, "name", 4) == 0 &&
            tz_error_named(object)) {
          name = field;
        }
        continue;
      }
      if (field->offset == TZ_FIELD_METHOD || field->offset == TZ_FIELD_GETTER ||
          tz_field_is_symbol(layout, field) || !has_json(field->type, fields + field->offset)) {
        continue;
      }
      separate(w, written++);
      quote_utf8(out, field->name, field->name_length);
      tz_buf_ascii(out, w->gap.length ? ": " : ":");
      write_value(w, field->type, fields + field->offset);
    }
    if (name) {
      separate(w, written++);
      quote_utf8(out, name->name, name->name_length);
      tz_buf_ascii(out, w->gap.length ? ": " : ":");
      write_value(w, name->type, fields + name->offset);
    }
    leave(w);
    close_with(w, written, '}');
    return 1;
  }
  default: // undefined, functions
    return 0;
  }
}

// JSON.stringify(value, null, space): the text in `out`, or 0 when the value
// has none (undefined, a function), which JavaScript returns as undefined.
// `space` is a number of spaces (`space_kind` 1), a string (2) or nothing. A
// value inside itself is a TypeError, and 0 with the empty string.
int32_t tz_json_stringify(TzStr *out, const TzType *type, const void *value, int32_t space_kind,
                          double space_number, void *space_obj, uint64_t space_meta) {
  Writer w;
  memset(&w, 0, sizeof w);
  *out = tz_str_from_ascii("", 0);
  if (__builtin_setjmp(w.fail)) {
    return 0; // enter released the writer
  }
  tz_buf_init(&w.out);
  tz_buf_init(&w.gap);
  tz_buf_init(&w.indent);
  // At most ten spaces or ten code units, as in JavaScript.
  if (space_kind == 1) {
    double n = space_number < 10 ? floor(space_number) : 10;
    for (int i = 0; i < (int)n; ++i) {
      tz_buf_push(&w.gap, ' ');
    }
  } else if (space_kind == 2) {
    TzStr s = {space_obj, space_meta};
    tz_buf_str(&w.gap, s);
    if (w.gap.length > 10) {
      w.gap.length = 10;
    }
  }
  int written = write_value(&w, type, value);
  if (written) {
    *out = tz_str_from_utf16(w.out.data, w.out.length);
  }
  writer_release(&w);

  return written;
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
    json_throw(TZ_TYPE_ERROR, &b);
    return -1;
  }
  switch (j->kind) {
  case TZ_JSON_OBJECT:
    return j->u.object.count;
  case TZ_JSON_ARRAY:
    return j->u.array->length;
  case TZ_JSON_STRING: {
    View v;
    view_init(&v, j->u.string);
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
      *value = retained(e->value);
    }
    return;
  }
  if (key) {
    *key = index_text(k);
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

//===----------------------------------------------------------------------===//
// JSON.parse<T> and fromJson<T>
//===----------------------------------------------------------------------===//

// The compiler makes a function for each type T that checks a Json and builds
// a T from it (CodeGen::fromJsonFunction); these are its steps. `path` says
// where the value is, for the TypeError when it does not fit:
// `address.city: expected string, got number`.
typedef struct TzJsonPath {
  const struct TzJsonPath *parent;
  void *key_obj; // a key of an object, or with `index` >= 0 an element
  uint64_t key_meta;
  int64_t index;
} TzJsonPath;

static int is_identifier(TzStr key) {
  View v;
  view_init(&v, key);
  if (v.length == 0) {
    return 0;
  }
  for (uint32_t i = 0; i < v.length; ++i) {
    uint16_t c = view_at(&v, i);
    int letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
    if (!letter && !(i > 0 && c >= '0' && c <= '9')) {
      return 0;
    }
  }

  return 1;
}

static void path_text(TzBuf *b, const TzJsonPath *path) {
  if (!path) {
    return;
  }
  path_text(b, path->parent);
  if (path->index >= 0) {
    char text[32];
    int n = snprintf(text, sizeof text, "[%lld]", (long long)path->index);
    tz_buf_ascii_n(b, text, (size_t)n);
    return;
  }
  TzStr key = {path->key_obj, path->key_meta};
  if (is_identifier(key)) {
    if (b->length) {
      tz_buf_push(b, '.');
    }
    tz_buf_str(b, key);
  } else {
    tz_buf_push(b, '[');
    quote(b, key);
    tz_buf_push(b, ']');
  }
}

// `got` describes what was there: its kind, or for a number the number. The
// caller returns next, with zero: the program checks tz_thrown after each
// step (CodeGen::fromJsonFunction).
static void convert_error(const TzJsonPath *path, const char *type, int64_t type_length,
                          const TzJson *got, int show_number) {
  TzBuf b;
  tz_buf_init(&b);
  path_text(&b, path);
  if (b.length) {
    tz_buf_ascii(&b, ": ");
  }
  tz_buf_ascii(&b, "expected ");
  tz_buf_ascii_n(&b, type, (size_t)type_length);
  tz_buf_ascii(&b, ", got ");
  if (show_number && got && got->kind == TZ_JSON_NUMBER) {
    char text[32];
    tz_buf_ascii_n(&b, text, tz_format_f64(got->u.number.value, text));
  } else {
    tz_buf_ascii(&b, kind_name(got));
  }
  json_throw(TZ_TYPE_ERROR, &b);
}

void tz_json_expect_object(const TzJson *j, const TzJsonPath *path, const char *type,
                           int64_t type_length) {
  if (!j || j->kind != TZ_JSON_OBJECT) {
    convert_error(path, type, type_length, j, 0);
  }
}

// The value of a key of an object, borrowed; NULL when it is not there.
TzJson *tz_json_field(const TzJson *object, void *key_obj, uint64_t key_meta) {
  TzStr key = {key_obj, key_meta};
  int64_t at = object_find(object, key);

  return at >= 0 ? object->u.object.entries[at].value : NULL;
}

// The entries of an object, for a Record<string, T>: how many, and the key
// and the value of each (borrowed), in the order of the text, which is the
// order the Record keeps.
uint32_t tz_json_object_count(const TzJson *object) { return object->u.object.count; }

TzJson *tz_json_object_entry(const TzJson *object, uint32_t k, TzStr *key) {
  const TzJsonEntry *e = &object->u.object.entries[k];
  *key = e->key;

  return e->value;
}

// The elements of an array, borrowed.
TzArray *tz_json_expect_array(const TzJson *j, const TzJsonPath *path, const char *type,
                              int64_t type_length) {
  if (!j || j->kind != TZ_JSON_ARRAY) {
    convert_error(path, type, type_length, j, 0);
    return NULL;
  }

  return j->u.array;
}

double tz_json_expect_double(const TzJson *j, const TzJsonPath *path) {
  if (!j || j->kind != TZ_JSON_NUMBER) {
    convert_error(path, "double", 6, j, 0);
    return 0;
  }

  return j->u.number.value;
}

// An int: a whole number in its range.
int32_t tz_json_expect_int(const TzJson *j, const TzJsonPath *path) {
  if (!j || j->kind != TZ_JSON_NUMBER) {
    convert_error(path, "int", 3, j, 0);
    return 0;
  }
  double d = j->u.number.value;
  if (!(d == floor(d) && d >= -2147483648.0 && d <= 2147483647.0)) {
    convert_error(path, "int", 3, j, 1);
    return 0;
  }

  return (int32_t)d;
}

// A long: a whole number in its range, exactly as written even beyond 2^53.
int64_t tz_json_expect_long(const TzJson *j, const TzJsonPath *path) {
  if (!j || j->kind != TZ_JSON_NUMBER) {
    convert_error(path, "long", 4, j, 0);
    return 0;
  }
  if (j->flags & F_TEXT) {
    View v;
    view_init(&v, j->u.number.text);
    uint32_t i = view_at(&v, 0) == '-';
    int negative = i == 1;
    uint64_t magnitude = 0;
    int ok = i < v.length;
    for (; i < v.length && ok; ++i) {
      uint16_t c = view_at(&v, i);
      ok = c >= '0' && c <= '9' && magnitude <= (UINT64_C(9223372036854775808) - (c - '0')) / 10;
      magnitude = magnitude * 10 + (c - '0');
    }
    // Only plain digits are read here; "1e3" and the like go by the double.
    if (ok && magnitude <= UINT64_C(9223372036854775807) + (uint64_t)negative) {
      return negative ? (int64_t)(0 - magnitude) : (int64_t)magnitude;
    }
  }
  double d = j->u.number.value;
  if (!(d == floor(d) && d >= -9223372036854775808.0 && d < 9223372036854775808.0)) {
    convert_error(path, "long", 4, j, 1);
    return 0;
  }

  return (int64_t)d;
}

int32_t tz_json_expect_bool(const TzJson *j, const TzJsonPath *path) {
  if (!j || j->kind != TZ_JSON_BOOL) {
    convert_error(path, "boolean", 7, j, 0);
    return 0;
  }

  return j->u.boolean;
}

void tz_json_expect_string(TzStr *out, const TzJson *j, const TzJsonPath *path) {
  if (!j || j->kind != TZ_JSON_STRING) {
    convert_error(path, "string", 6, j, 0);
    *out = tz_str_from_ascii("", 0);
    return;
  }
  *out = j->u.string;
  tz_str_retain_value(*out);
}

// A decimal from the text of the number, exactly (docs/decimal.md); a number
// that kept no text has at most 15 digits, which the double gives back.
void tz_json_expect_decimal(TzDec *out, const TzJson *j, const TzJsonPath *path) {
  if (!j || j->kind != TZ_JSON_NUMBER) {
    convert_error(path, "decimal", 7, j, 0);
    memset(out, 0, sizeof *out);
    return;
  }
  TzStr text;
  if (j->flags & F_TEXT) {
    text = j->u.number.text;
    tz_str_retain_value(text);
  } else {
    tz_str_from_f64(&text, j->u.number.value == 0 ? 0 : j->u.number.value);
  }
  tz_dec_from_string(out, text.obj, text.meta);
  tz_str_release_value(text);
}

// A Json taken as it is: anything but undefined. Owned.
TzJson *tz_json_expect_value(const TzJson *j, const TzJsonPath *path) {
  if (!j) {
    convert_error(path, "Json", 4, j, 0);
    return NULL;
  }

  return retained((TzJson *)j);
}

// Literal types and unions. Whether `j` is the value of a literal type: the
// string of `obj`/`meta`, a number, or a boolean.
int32_t tz_json_is_string(const TzJson *j, void *obj, uint64_t meta) {
  return j && j->kind == TZ_JSON_STRING &&
         tz_str_equal(j->u.string.obj, j->u.string.meta, obj, meta);
}

int32_t tz_json_is_number(const TzJson *j, double value) {
  return j && j->kind == TZ_JSON_NUMBER && j->u.number.value == value;
}

int32_t tz_json_is_bool(const TzJson *j, int32_t value) {
  return j && j->kind == TZ_JSON_BOOL && j->u.boolean == value;
}

// A value that no member of a union takes, or that a literal type does not
// have: what is there, with the value of a string or a number, as in
// `kind: expected "circle" | "square", got "triangle"`.
void tz_json_mismatch(const TzJson *j, const TzJsonPath *path, const char *type,
                      int64_t type_length) {
  if (!j || j->kind != TZ_JSON_STRING) {
    convert_error(path, type, type_length, j, 1);
    return;
  }
  TzBuf b;
  tz_buf_init(&b);
  path_text(&b, path);
  if (b.length) {
    tz_buf_ascii(&b, ": ");
  }
  tz_buf_ascii(&b, "expected ");
  tz_buf_ascii_n(&b, type, (size_t)type_length);
  tz_buf_ascii(&b, ", got ");
  quote(&b, j->u.string);
  json_throw(TZ_TYPE_ERROR, &b);
}
