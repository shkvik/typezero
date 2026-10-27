#include "runtime/library/json/json/json.h"
#include "runtime/core/output/output.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/array/array.h"
#include "runtime/core/error/error.h"
#include "runtime/core/group/group.h"
#include "runtime/core/parallel/parallel.h"
#include "runtime/core/publish/publish.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include <math.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define F_INDEX_KEYS 1u

#define INDEX_FROM 8

#define IMMORTAL (INT64_C(1) << 62)

TzJson tz_json_null_node = {IMMORTAL, TZ_JSON_NULL, 0, {0}};

TzJson tz_json_true_node = {IMMORTAL, TZ_JSON_BOOL, 0, {1}};

TzJson tz_json_false_node = {IMMORTAL, TZ_JSON_BOOL, 0, {0}};

const TzType tz_json_type = {TZ_T_JSON, sizeof(TzJson *), NULL};

int64_t tz_json_live_objects(void) { return tz_thread_sum(offsetof(TzThread, live_json)); }

TzJson *tz_json_node_new(uint32_t kind, size_t payload) {
  TzJson *j = (TzJson *)tz_alloc(offsetof(TzJson, u) + payload);
  j->rc = 1;
  j->kind = kind;
  j->flags = 0;
  ++tz_live_json;

  return j;
}

TzJson *tz_json_retained(TzJson *j) {
  tz_rc_inc(&j->rc);

  return j;
}

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
void tz_json_publish(TzJson *root) {
  if (!root) {
    return;
  }
  Dead pending = {NULL, 0, 0};
  dead_push(&pending, root);
  while (pending.count) {
    TzJson *j = pending.items[--pending.count];
    if (j->rc <= 0 || j->rc >= TZ_RC_FOREVER) {
      continue;
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
    default:
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

TZ_HOT void tz_json_release_value(TzJson *j) {
  if (j && tz_rc_dec(&j->rc)) {
    tz_json_free(j);
  }
}

TzMaker tz_json_error_maker;

void tz_json_errors(TzMaker make) { tz_json_error_maker = make; }

typedef struct {
  const char *code;
  TzStr message;
  TzStr path;
} ErrorParts;

void tz_json_fail(const char *code, TzBuf *message, TzBuf *path) {
  if (!tz_json_error_maker && strcmp(code, "circular") == 0) {
    tz_panicf("TypeError: Converting circular structure to JSON");
  }
  if (!tz_json_error_maker) {
    tz_panicf("internal error: a Json error with nothing to take it (%s)", code);
  }
  ErrorParts parts;
  parts.code = code;
  parts.message = tz_buf_take(message);
  if (path) {
    parts.path = tz_buf_take(path);
  } else {
    TzBuf empty;
    tz_buf_init(&empty);
    parts.path = tz_buf_take(&empty);
  }
  tz_failed = tz_json_error_maker(&parts);
}

void tz_json_view_init(View *v, TzStr s) {
  v->flat = s;
  v->units = tz_str_units(&v->flat, &v->length, &v->two);
}

uint16_t tz_json_view_at(const View *v, uint32_t i) {
  return v->two ? ((const uint16_t *)v->units)[i] : ((const uint8_t *)v->units)[i];
}

static uint32_t key_hash(TzStr key) {
  View v;
  tz_json_view_init(&v, key);
  uint32_t h = 2166136261u;
  for (uint32_t i = 0; i < v.length; ++i) {
    h ^= tz_json_view_at(&v, i);
    h *= 16777619u;
  }

  return h;
}

int tz_json_key_equal(TzStr a, TzStr b) { return tz_str_equal(a.obj, a.meta, b.obj, b.meta); }

int64_t tz_json_index_key(TzStr key) {
  View v;
  tz_json_view_init(&v, key);
  if (v.length == 0 || v.length > 10 || (v.length > 1 && tz_json_view_at(&v, 0) == '0')) {
    return -1;
  }
  int64_t n = 0;
  for (uint32_t i = 0; i < v.length; ++i) {
    uint16_t c = tz_json_view_at(&v, i);
    if (c < '0' || c > '9') {
      return -1;
    }
    n = n * 10 + (c - '0');
  }

  return n <= INT64_C(4294967294) ? n : -1;
}

int tz_json_is_length_key(TzStr key) {
  TzStr length = tz_str_from_ascii("length", 6);

  return tz_json_key_equal(key, length);
}

TzStr tz_json_index_text(uint32_t i) {
  char text[16];
  int n = snprintf(text, sizeof text, "%u", i);

  return tz_str_from_ascii(text, (size_t)n);
}

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

int64_t tz_json_object_find(const TzJson *o, TzStr key) {
  const TzJsonEntry *entries = o->u.object.entries;
  if (!o->u.object.index) {
    for (uint32_t i = 0; i < o->u.object.count; ++i) {
      if (tz_json_key_equal(entries[i].key, key)) {
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
    if (tz_json_key_equal(entries[found - 1].key, key)) {
      return found - 1;
    }
  }
}

void tz_json_put(TzJson *o, TzStr key, TzJson *value) {
  int64_t at = tz_json_object_find(o, key);
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
  uint32_t length;
  int two;
  tz_str_units(&key, &length, &two);
  tz_str_retain_value(key);
  uint32_t position = o->u.object.count++;
  o->u.object.entries[position].key = key;
  o->u.object.entries[position].value = value;
  if (tz_json_index_key(key) >= 0) {
    o->flags |= F_INDEX_KEYS;
  }
  if (o->u.object.index && o->u.object.count * 2 + 2 <= o->u.object.index_size) {
    index_insert(o->u.object.index, o->u.object.index_size, key_hash(key), position);
  } else if (o->u.object.count > INDEX_FROM) {
    index_build(o);
  }
}

void tz_json_object_remove(TzJson *o, TzStr key) {
  int64_t at = tz_json_object_find(o, key);
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
    int64_t index = tz_json_index_key(o->u.object.entries[i].key);
    if (index >= 0) {
      indices[n++] = (IndexKey){index, i};
    }
  }
  qsort(indices, n, sizeof(IndexKey), compare_index_keys);
  for (uint32_t i = 0; i < n; ++i) {
    order[k++] = indices[i].position;
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (tz_json_index_key(o->u.object.entries[i].key) < 0) {
      order[k++] = i;
    }
  }
  tz_free(indices);

  return order;
}
