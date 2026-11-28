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

TZ_THREAD_LOCAL TzMaker tz_json_error_maker;

typedef struct {
  const char *code;
  TzStr message;
} ErrorParts;

static void index_insert(uint32_t *index, uint32_t size, uint32_t hash, uint32_t position) {
  uint32_t at = hash & (size - 1);
  while (index[at]) {
    at = (at + 1) & (size - 1);
  }
  index[at] = position + 1;
}

typedef struct {
  int64_t index;
  uint32_t position;
} IndexKey;

