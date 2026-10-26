#include "runtime/core/publish/publish.h"
#include "runtime/core/array/array.h"
#include "runtime/core/async/async.h"
#include "runtime/core/closure/closure.h"
#include "runtime/core/group/group.h"
#include "runtime/core/map/map.h"
#include "runtime/core/output/output.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include "runtime/core/unknown/unknown.h"
#include "runtime/library/bytes/bytes/bytes.h"
#include "runtime/library/decimal/decimal/decimal.h"
#include "runtime/library/json/json/json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void tz_rc_shared_add(int64_t *rc, int64_t n) {
#if TZ_PARALLEL
  __atomic_fetch_add(rc, n, __ATOMIC_RELAXED);
#else
  *rc += n;
#endif
}

int tz_rc_shared_dec(int64_t *rc) {
#if TZ_PARALLEL
  if (__atomic_sub_fetch(rc, 1, __ATOMIC_ACQ_REL) != TZ_RC_SHARED) {
    return 0;
  }
  *rc = 0;

  return 1;
#else

  return --*rc == 0;
#endif
}

#if TZ_PARALLEL

void tz_str_publish(TzStr s);
void tz_json_publish(TzJson *j);
void tz_dec_publish(TzDec d);
void tz_map_publish(TzMap *m);

typedef struct {
  const void *descriptor;
  void (*publish)(void *object);
} TzPublishEntry;

const TzPublishEntry **tz_publish_slots;
uint32_t tz_publish_mask;

static uint32_t descriptor_hash(const void *d) {
  uint64_t h = (uint64_t)(uintptr_t)d * UINT64_C(0x9E3779B97F4A7C15);

  return (uint32_t)(h >> 32);
}

void tz_publish_init(const TzPublishEntry *entries, uint32_t count) {
  uint32_t size = 16;
  while (size < count * 2) {
    size *= 2;
  }
  tz_publish_slots = (const TzPublishEntry **)calloc(size, sizeof *tz_publish_slots);
  if (!tz_publish_slots) {
    tz_panic("out of memory", 13);
  }
  tz_publish_mask = size - 1;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t at = descriptor_hash(entries[i].descriptor) & tz_publish_mask;
    while (tz_publish_slots[at] && tz_publish_slots[at]->descriptor != entries[i].descriptor) {
      at = (at + 1) & tz_publish_mask;
    }
    tz_publish_slots[at] = &entries[i];
  }
}

static void (*publisher_of(const void *descriptor))(void *) {
  if (!tz_publish_slots) {
    return NULL;
  }
  for (uint32_t at = descriptor_hash(descriptor) & tz_publish_mask;;
       at = (at + 1) & tz_publish_mask) {
    const TzPublishEntry *e = tz_publish_slots[at];
    if (!e) {
      return NULL;
    }
    if (e->descriptor == descriptor) {
      return e->publish;
    }
  }
}

typedef struct {
  void **items;
  size_t count, capacity;
  int active;
} Pending;

TZ_THREAD_LOCAL Pending tz_publish_pending;
#define pending tz_publish_pending

static void pending_push(void *object) {
  if (pending.count == pending.capacity) {
    size_t capacity = pending.capacity ? pending.capacity * 2 : 64;
    void **items = (void **)realloc(pending.items, capacity * sizeof *items);
    if (!items) {
      tz_panic("out of memory", 13);
    }
    pending.items = items;
    pending.capacity = capacity;
  }
  pending.items[pending.count++] = object;
}

static void push_member(void *member, int32_t array) {
  pending_push((void *)((uintptr_t)member | (array ? 1 : 0)));
}

static int mark(void *object) {
  int64_t *rc = (int64_t *)object;
  int64_t count = *rc;
  if (count > 0 && count < TZ_RC_FOREVER) {
    *rc = TZ_RC_SHARED + count;
    return 1;
  }

  return 0;
}

static void publish_ref(void *object) {
  if (!object) {
    return;
  }
  int64_t rc = ((TzObject *)object)->rc;
  if (tz_rc_is_member(rc)) {
    tz_group_publish(object, push_member);
    return;
  }
  if (mark(object)) {
    pending_push(object);
  }
}

static void publish_array_contents(TzArray *a);

static void publish_contents(void *item) {
  if ((uintptr_t)item & 1) {
    publish_array_contents((TzArray *)((uintptr_t)item & ~(uintptr_t)1));
    return;
  }
  void *object = item;
  const TzLayout *layout = ((TzInstance *)object)->layout;
  void (*publish)(void *) = publisher_of(layout);
  if (publish) {
    publish(object);
    return;
  }
  uint32_t flags = layout->flags;
  if (flags & TZ_LAYOUT_DICT) {
    tz_map_publish((TzMap *)object);
  } else if (flags & TZ_LAYOUT_PROMISE) {
    if (layout->count) {
      tz_publish_value(layout->fields[0].type, (char *)object + TZ_PROMISE_VALUE);
    }
    TzPromise *p = (TzPromise *)object;
    if (p->state == TZ_PROMISE_REJECTED) {
      publish_ref(p->error);
    }
  } else if (flags & TZ_LAYOUT_BYTES) {
    if (tz_bytes_kind(object) != TZ_BYTES_ARRAY_BUFFER) {
      publish_ref(((TzBytes *)object)->buffer);
    }
  } else if (flags & TZ_LAYOUT_BOX) {
    TzBox *box = (TzBox *)object;
    tz_publish_value(box->type, box + 1);
  } else if (flags & TZ_LAYOUT_TIMER) {
  } else {
    if (getenv("TZ_PUBLISH_DEBUG")) {
      fprintf(stderr, "publish fallback: object %p layout %p drop %p count %u flags %x\n", object,
              (const void *)layout, (void *)layout->drop, layout->count, layout->flags);
      fflush(stderr);
    }
    for (uint32_t i = 0; i < layout->count; ++i) {
      const TzField *f = &layout->fields[i];
      if (f->offset != TZ_FIELD_METHOD && f->offset != TZ_FIELD_GETTER) {
        tz_publish_value(f->type, (char *)object + f->offset);
      }
    }
  }
}

static void publish_array_contents(TzArray *a) {
  if (!tz_is_managed(a->type)) {
    return;
  }
  size_t size = a->type->size;
  for (uint32_t i = 0; i < a->length; ++i) {
    tz_publish_value(a->type, a->data + (size_t)i * size);
  }
}

static void drain(void) {
  if (pending.active) {
    return;
  }
  pending.active = 1;
  while (pending.count) {
    publish_contents(pending.items[--pending.count]);
  }
  pending.active = 0;
}

void tz_publish_contents(void *member, int32_t array) {
  push_member(member, array);
  drain();
}

static void publish_array(TzArray *a) {
  if (!a) {
    return;
  }
  if (tz_rc_is_member(a->rc)) {
    tz_group_publish(a, push_member);
    return;
  }
  if (!mark(a)) {
    return;
  }
  publish_array_contents(a);
}

void tz_publish_value(const TzType *type, void *value) {
  if (type->kind == TZ_T_UNION) {
    const void *held = value;
    type = tz_resolve(type, &held);
    value = (void *)held;
  }
  switch (type->kind) {
  case TZ_T_STRING:
    tz_str_publish(*(TzStr *)value);
    break;
  case TZ_T_ARRAY:
    publish_array(*(TzArray **)value);
    break;
  case TZ_T_OPTIONAL:
    if (tz_optional_has(type, value)) {
      tz_publish_value(type->inner, value);
    }
    break;
  case TZ_T_FUNCTION:
    publish_ref(((TzFunction *)value)->env);
    break;
  case TZ_T_OBJECT:
  case TZ_T_UNKNOWN:
    publish_ref(*(void **)value);
    break;
  case TZ_T_JSON:
    tz_json_publish(*(TzJson **)value);
    break;
  case TZ_T_DECIMAL:
    tz_dec_publish(*(TzDec *)value);
    break;
  case TZ_T_RESULT: {
    const TzResultType *result = (const TzResultType *)type;
    tz_publish_value(type->inner, (char *)value + result->value_offset);
    tz_publish_value(result->error, (char *)value + result->error_offset);
    break;
  }
  default:
    break;
  }
  drain();
}

void tz_publish_object(void *object) {
  publish_ref(object);
  drain();
}

void tz_store_barrier(void *owner, const TzType *type, void *value) {
  int64_t rc = ((TzObject *)owner)->rc;
  if (tz_rc_is_shared(rc) || (tz_rc_is_member(rc) && tz_group_published(owner))) {
    tz_publish_value(type, value);
  }
}

void tz_store_barrier_ref(void *owner, void *object) {
  int64_t rc = ((TzObject *)owner)->rc;
  if (tz_rc_is_shared(rc) || (tz_rc_is_member(rc) && tz_group_published(owner))) {
    tz_publish_object(object);
  }
}

#else

void tz_publish_init(const void *entries, uint32_t count) {
  (void)entries;
  (void)count;
}
void tz_publish_value(const TzType *type, void *value) {
  (void)type;
  (void)value;
}
void tz_publish_object(void *object) { (void)object; }
void tz_store_barrier(void *owner, const TzType *type, void *value) {
  (void)owner;
  (void)type;
  (void)value;
}
void tz_store_barrier_ref(void *owner, void *object) {
  (void)owner;
  (void)object;
}

#endif
