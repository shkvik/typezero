#include "runtime/core/group/group.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/array/array.h"
#include "runtime/core/closure/closure.h"
#include "runtime/core/error/error.h"
#include "runtime/core/map/map.h"
#include "runtime/core/output/output.h"
#include "runtime/core/parallel/parallel.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if TZ_PARALLEL
int32_t tz_group_lock_owner;
int32_t tz_group_lock_depth;

static inline void group_lock(void) {
  int32_t me = tz_thread.index + 1;
  if (__atomic_load_n(&tz_group_lock_owner, __ATOMIC_RELAXED) == me) {
    ++tz_group_lock_depth;
    return;
  }
  for (unsigned spin = 0;; ++spin) {
    int32_t none = 0;
    if (__atomic_compare_exchange_n(&tz_group_lock_owner, &none, me, 1, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED)) {
      break;
    }
    if (spin > 64) {
      tz_thread_yield();
    } else {
      __builtin_ia32_pause();
    }
  }
  tz_group_lock_depth = 1;
}

static inline void group_unlock(void) {
  if (--tz_group_lock_depth == 0) {
    __atomic_store_n(&tz_group_lock_owner, 0, __ATOMIC_RELEASE);
  }
}
#else
#define group_lock() ((void)0)
#define group_unlock() ((void)0)
#endif

int32_t tz_group_listing;

typedef struct Listed {
  struct Listed *prev, *next;
  int64_t outside;
  int64_t size;
} Listed;

Listed tz_group_list = {&tz_group_list, &tz_group_list, 0, 0};

static inline Listed *listed(const void *member) {
  return (Listed *)((char *)member - sizeof(TzGroupLink) - sizeof(Listed));
}

int32_t tz_group_debugging;
TZ_THREAD_LOCAL const TzGroupSite *tz_group_site;
TZ_NOINLINE void tz_group_merged(TzObject *leader, int64_t joined);
TZ_NOINLINE void tz_group_check_unstored(const void *object);

#define LEADER_PUBLISHED ((uintptr_t)1)

static inline TzObject *leader_of(const void *member) {
  return (TzObject *)((uintptr_t)tz_group_link(member)->leader & ~LEADER_PUBLISHED);
}

#if TZ_PARALLEL
static inline int member_published(const void *member) {
  void *leader = __atomic_load_n(&tz_group_link(member)->leader, __ATOMIC_RELAXED);

  return ((uintptr_t)leader & LEADER_PUBLISHED) != 0;
}

static inline int published(const TzObject *leader) { return member_published(leader); }

static inline int lock_for(const void *a, const void *b) {
  if (!tz_group_listing && !member_published(a) && (!b || !member_published(b))) {
    return 0;
  }
  group_lock();

  return 1;
}

static inline void unlock_for(int locked) {
  if (locked) {
    group_unlock();
  }
}
#else
#define lock_for(a, b) 0
#define unlock_for(locked) ((void)(locked))
#endif

static inline void *untagged(uintptr_t next) { return (void *)(next & ~TZ_GROUP_ARRAY); }

static inline void *next_of(const void *member) { return untagged(tz_group_link(member)->next); }

static inline int is_eternal(const TzObject *leader) { return leader->rc >= TZ_RC_ETERNAL; }

static inline size_t front_size(void) {
  return sizeof(TzGroupLink) + (tz_group_listing ? sizeof(Listed) : 0);
}

TZ_NOINLINE void *tz_group_alloc_listed(size_t size, uintptr_t tag) {
  Listed *l = (Listed *)tz_alloc(size + sizeof(Listed) + sizeof(TzGroupLink));
  l->prev = &tz_group_list;
  l->next = tz_group_list.next;
  tz_group_list.next->prev = l;
  tz_group_list.next = l;
  l->outside = 1;
  l->size = 1;
  char *member = (char *)l + sizeof(Listed) + sizeof(TzGroupLink);
#if TZ_PARALLEL
  memset(member, 0, size);
#endif
  TzGroupLink *link = tz_group_link(member);
  link->leader = member;
  link->next = (uintptr_t)member | tag;
  ((TzObject *)member)->rc = TZ_RC_GROUP + 1;

  return member;
}

TZ_HOT void *tz_group_alloc_tagged(size_t size, uintptr_t tag) {
  if (tz_group_listing) {
    group_lock();
    void *member = tz_group_alloc_listed(size, tag);
    group_unlock();
    return member;
  }
  char *member = (char *)tz_alloc(size + sizeof(TzGroupLink)) + sizeof(TzGroupLink);
#if TZ_PARALLEL
  memset(member, 0, size);
#endif
  TzGroupLink *link = tz_group_link(member);
  link->leader = member;
  link->next = (uintptr_t)member | tag;
  ((TzObject *)member)->rc = TZ_RC_GROUP + 1;

  return member;
}

TZ_HOT void *tz_group_alloc(int64_t size) {
  ++tz_live_objects;

  return tz_group_alloc_tagged((size_t)size, 0);
}

static void member_free(void *member) {
  char *block = (char *)member - front_size();
  if (tz_group_listing) {
    group_lock();
    Listed *l = (Listed *)block;
    l->prev->next = l->next;
    l->next->prev = l->prev;
    group_unlock();
  }
  tz_free(block);
}

TZ_HOT void tz_group_obj_free(void *object) {
  if (!tz_rc_is_member(((TzObject *)object)->rc)) {
    if (tz_group_debugging) {
      tz_group_forget_unstored(object);
    }
    tz_obj_free(object);
  }
}

#if TZ_PARALLEL
TZ_NOINLINE void tz_group_retain_locked(void *member) {
  group_lock();
  ++leader_of(member)->rc;
  if (tz_group_listing) {
    ++listed(member)->outside;
  }
  group_unlock();
}

TZ_NOINLINE void tz_group_release_locked(void *member) {
  group_lock();
  TzObject *leader = leader_of(member);
  if (tz_group_listing) {
    --listed(member)->outside;
  }
  int dead = --leader->rc == TZ_RC_GROUP;
  if (dead) {
    leader->rc = TZ_RC_DYING;
  }
  group_unlock();
  if (dead) {
    tz_group_free(leader);
  }
}

TZ_HOT void tz_group_retain(void *member) {
  int64_t *rc = &((TzObject *)member)->rc;
  if (*rc >= TZ_RC_SHARED) {
    __atomic_fetch_add(rc, 1, __ATOMIC_RELAXED);
  } else if (!tz_group_listing && !member_published(member)) {
    ++leader_of(member)->rc;
  } else {
    tz_group_retain_locked(member);
  }
}

TZ_HOT void tz_group_release(void *member) {
  TzObject *o = (TzObject *)member;
  if (o->rc >= TZ_RC_SHARED) {
    if (tz_rc_dec(&o->rc)) {
      o->desc->drop(o);
    }
  } else if (!tz_group_listing && !member_published(member)) {
    TzObject *leader = leader_of(member);
    if (--leader->rc == TZ_RC_GROUP) {
      tz_group_free(leader);
    }
  } else {
    tz_group_release_locked(member);
  }
}
#else
TZ_HOT void tz_group_retain(void *member) {
  ++leader_of(member)->rc;
  if (tz_group_listing) {
    ++listed(member)->outside;
  }
}

TZ_HOT void tz_group_release(void *member) {
  TzObject *leader = leader_of(member);
  if (tz_group_listing) {
    --listed(member)->outside;
  }
  if (--leader->rc == TZ_RC_GROUP) {
    tz_group_free(leader);
  }
}
#endif

#if TZ_PARALLEL
void tz_publish_contents(void *member, int32_t array);

static void each_member(TzObject *leader, void (*each)(void *member, int32_t array)) {
  uintptr_t at = tz_group_link(leader)->next;
  for (;;) {
    void *m = untagged(at);
    uintptr_t following = tz_group_link(m)->next;
    each(m, (at & TZ_GROUP_ARRAY) != 0);
    if (m == leader) {
      break;
    }
    at = following;
  }
}

static void publish_member(void *member, int32_t array) {
  TzGroupLink *link = tz_group_link(member);
  link->leader = (void *)((uintptr_t)link->leader | LEADER_PUBLISHED);
  tz_publish_contents(member, array);
}

static void publish_ring(TzObject *leader) { each_member(leader, publish_member); }
#endif

static TzObject *merge(TzObject *a, TzObject *b) {
  void *x = a, *y = b;
  for (;;) {
    x = next_of(x);
    y = next_of(y);
    if (x == a) {
      break;
    }
    if (y == b) {
      TzObject *t = a;
      a = b;
      b = t;
      break;
    }
  }
  void *joined = b;
#if TZ_PARALLEL
  int pa = published(a), pb = published(b);
  if (pa != pb) {
    publish_ring(pa ? b : a);
  }
  if (pa || pb) {
    joined = (void *)((uintptr_t)b | LEADER_PUBLISHED);
  }
#endif
  void *m = a;
  do {
    tz_group_link(m)->leader = joined;
    m = next_of(m);
  } while (m != a);
  uintptr_t t = tz_group_link(a)->next;
  tz_group_link(a)->next = tz_group_link(b)->next;
  tz_group_link(b)->next = t;
  b->rc += a->rc - TZ_RC_GROUP;
  a->rc = TZ_RC_MEMBER;
  if (tz_group_listing) {
    listed(b)->size += listed(a)->size;
    if (tz_group_debugging) {
      tz_group_merged(b, listed(a)->size);
    }
  }

  return b;
}

TZ_NOINLINE void tz_group_link_groups(TzObject *owner, TzObject *leader, void *ref) {
  if (is_eternal(leader)) {
    --leader->rc;
    if (tz_group_listing) {
      --listed(ref)->outside;
    }
    return;
  }
  if (is_eternal(owner)) {
    return;
  }
  --merge(owner, leader)->rc;
  if (tz_group_listing) {
    --listed(ref)->outside;
  }
}

TZ_HOT void tz_group_store(void *owner, void *ref) {
  if (!ref) {
    return;
  }
  if (!tz_rc_is_member(((TzObject *)ref)->rc)) {
    if (tz_group_debugging) {
      tz_group_check_unstored(ref);
    }
    return;
  }
  if (!tz_rc_is_member(((TzObject *)owner)->rc)) {
    return;
  }
  int locked = lock_for(owner, ref);
  TzObject *lo = leader_of(owner), *lr = leader_of(ref);
  if (lo == lr) {
    --lr->rc;
    if (tz_group_listing) {
      --listed(ref)->outside;
    }
  } else {
    tz_group_link_groups(lo, lr, ref);
  }
  unlock_for(locked);
}

TZ_HOT void tz_group_unstore(void *owner, void *ref) {
#if TZ_PARALLEL
  if (!tz_rc_is_member(((TzObject *)ref)->rc)) {
    tz_group_release(ref);
    return;
  }
#endif
  int member = tz_rc_is_member(((TzObject *)owner)->rc);
  int locked = lock_for(ref, member ? owner : NULL);
  TzObject *lr = leader_of(ref);
  if (member && (leader_of(owner) == lr || is_eternal(lr))) {
    unlock_for(locked);
    return;
  }
  if (tz_group_listing) {
    --listed(ref)->outside;
  }
  int dead = --lr->rc == TZ_RC_GROUP;
  if (dead) {
    lr->rc = TZ_RC_DYING;
  }
  unlock_for(locked);
  if (dead) {
    tz_group_free(lr);
  }
}

int32_t tz_group_store_within(void *owner, void *ref) {
  if (!ref || !tz_rc_is_member(((TzObject *)owner)->rc) ||
      !tz_rc_is_member(((TzObject *)ref)->rc)) {
    return 0;
  }
  int locked = lock_for(owner, ref);
  TzObject *lr = leader_of(ref);
  int within = leader_of(owner) == lr;
  if (within) {
    --lr->rc;
    if (tz_group_listing) {
      --listed(ref)->outside;
    }
  }
  unlock_for(locked);

  return within;
}

void tz_group_take(void *owner, void *ref) {
  if (!tz_rc_is_member(((TzObject *)owner)->rc) || !tz_rc_is_member(((TzObject *)ref)->rc)) {
    return;
  }
  int locked = lock_for(owner, ref);
  TzObject *lr = leader_of(ref);
  if (leader_of(owner) == lr || is_eternal(lr)) {
    ++lr->rc;
    if (tz_group_listing) {
      ++listed(ref)->outside;
    }
  }
  unlock_for(locked);
}

#if TZ_PARALLEL
void tz_group_publish(void *member, void (*each)(void *member, int32_t array)) {
  if (member_published(member)) {
    return;
  }
  group_lock();
  TzObject *leader = leader_of(member);
  uintptr_t at = tz_group_link(leader)->next;
  for (;;) {
    void *m = untagged(at);
    TzGroupLink *link = tz_group_link(m);
    uintptr_t following = link->next;
    link->leader = (void *)((uintptr_t)leader | LEADER_PUBLISHED);
    each(m, (at & TZ_GROUP_ARRAY) != 0);
    if (m == leader) {
      break;
    }
    at = following;
  }
  group_unlock();
}

int32_t tz_group_published(const void *member) { return member_published(member); }

#endif

static void each_reference(const TzType *type, void *value, void *owner,
                           void (*each)(void *owner, void *ref)) {
  if (type->kind == TZ_T_UNION) {
    const void *held = value;
    type = tz_resolve(type, &held);
    value = (void *)held;
  }
  switch (type->kind) {
  case TZ_T_ARRAY:
  case TZ_T_OBJECT:
  case TZ_T_UNKNOWN:
    if (*(void **)value) {
      each(owner, *(void **)value);
    }
    break;
  case TZ_T_FUNCTION:
    if (((TzFunction *)value)->env) {
      each(owner, ((TzFunction *)value)->env);
    }
    break;
  case TZ_T_OPTIONAL:
    each_reference(type->inner, value, owner, each);
    break;
  case TZ_T_RESULT: {
    const TzResultType *result = (const TzResultType *)type;
    each_reference(type->inner, (char *)value + result->value_offset, owner, each);
    each_reference(result->error, (char *)value + result->error_offset, owner, each);
    break;
  }
  default:
    break;
  }
}

static void store_one(void *owner, void *ref) { tz_group_store(owner, ref); }

static void take_one(void *owner, void *ref) {
  if (tz_rc_is_member(((TzObject *)ref)->rc)) {
    tz_group_take(owner, ref);
  }
}

static void pin_one(void *owner, void *ref) {
  (void)owner;
  if (!tz_rc_is_member(((TzObject *)ref)->rc)) {
    return;
  }
  group_lock();
  TzObject *leader = leader_of(ref);
  if (leader->rc < TZ_RC_ETERNAL) {
    leader->rc += TZ_RC_ETERNAL - TZ_RC_GROUP;
  }
  group_unlock();
}

void tz_group_store_value(void *owner, const TzType *type, void *value) {
  if (tz_rc_is_member(((TzObject *)owner)->rc) || tz_group_debugging) {
    each_reference(type, value, owner, store_one);
  }
}

void tz_group_take_value(void *owner, const TzType *type, void *value) {
  if (tz_rc_is_member(((TzObject *)owner)->rc)) {
    each_reference(type, value, owner, take_one);
  }
}

void tz_group_pin_value(const TzType *type, void *value) {
  each_reference(type, value, NULL, pin_one);
}

typedef struct TzWeak {
  int64_t rc;
  const TzLayout *layout;
  void *target;
  struct TzWeak *next;
} TzWeak;

typedef struct {
  void *key;
  TzWeak *first;
} WeakSlot;

WeakSlot *tz_weak_slots;
uint32_t tz_weak_capacity, tz_weak_used;
int64_t tz_weak_count;

#define TOMBSTONE ((void *)1)

static inline uint32_t weak_hash(const void *key) {
  uint64_t h = (uint64_t)(uintptr_t)key * UINT64_C(0x9E3779B97F4A7C15);

  return (uint32_t)(h >> 32);
}

static WeakSlot *weak_find(const void *key) {
  if (!tz_weak_capacity) {
    return NULL;
  }
  uint32_t mask = tz_weak_capacity - 1;
  for (uint32_t i = weak_hash(key) & mask;; i = (i + 1) & mask) {
    WeakSlot *s = &tz_weak_slots[i];
    if (s->key == key) {
      return s;
    }
    if (!s->key) {
      return NULL;
    }
  }
}

static void weak_grow(void) {
  uint32_t old = tz_weak_capacity;
  WeakSlot *slots = tz_weak_slots;
  tz_weak_capacity = old ? old * 2 : 16;
  tz_weak_slots = (WeakSlot *)tz_alloc(tz_weak_capacity * sizeof(WeakSlot));
  memset(tz_weak_slots, 0, tz_weak_capacity * sizeof(WeakSlot));
  tz_weak_used = 0;
  uint32_t mask = tz_weak_capacity - 1;
  for (uint32_t k = 0; k < old; ++k) {
    if (!slots[k].key || slots[k].key == TOMBSTONE) {
      continue;
    }
    uint32_t i = weak_hash(slots[k].key) & mask;
    while (tz_weak_slots[i].key) {
      i = (i + 1) & mask;
    }
    tz_weak_slots[i] = slots[k];
    ++tz_weak_used;
  }
  tz_free(slots);
}

void *tz_weak_new(const TzLayout *layout, void *target) {
  TzWeak *w = (TzWeak *)tz_obj_alloc(sizeof(TzWeak));
  w->rc = 1;
  w->layout = layout;
  w->target = target;
  group_lock();
  WeakSlot *s = weak_find(target);
  if (!s) {
    if ((tz_weak_used + 1) * 4 > tz_weak_capacity * 3) {
      weak_grow();
    }
    uint32_t mask = tz_weak_capacity - 1;
    uint32_t i = weak_hash(target) & mask;
    while (tz_weak_slots[i].key && tz_weak_slots[i].key != TOMBSTONE) {
      i = (i + 1) & mask;
    }
    s = &tz_weak_slots[i];
    if (!s->key) {
      ++tz_weak_used;
    }
    s->key = target;
    s->first = NULL;
    ++tz_weak_count;
  }
  w->next = s->first;
  s->first = w;
  group_unlock();

  return w;
}

void *tz_weak_deref(void *weak) {
  group_lock();
  void *target = ((TzWeak *)weak)->target;
  if (target) {
    tz_ref_retain(target);
  }
  group_unlock();

  return target;
}

TZ_NOINLINE void tz_weak_forget(void *target) {
  group_lock();
  WeakSlot *s = weak_find(target);
  if (!s) {
    group_unlock();
    return;
  }
  for (TzWeak *w = s->first; w; w = w->next) {
    w->target = NULL;
  }
  s->key = TOMBSTONE;
  s->first = NULL;
  --tz_weak_count;
  group_unlock();
}

void tz_weak_drop(void *object) {
  TzWeak *w = (TzWeak *)object;
  group_lock();
  if (w->target) {
    WeakSlot *s = weak_find(w->target);
    TzWeak **at = &s->first;
    while (*at != w) {
      at = &(*at)->next;
    }
    *at = w->next;
    if (!s->first) {
      s->key = TOMBSTONE;
      --tz_weak_count;
    }
  }
  group_unlock();
  tz_obj_free(w);
}

TZ_THREAD_LOCAL TzObject **tz_group_dead;
TZ_THREAD_LOCAL size_t tz_group_dead_count, tz_group_dead_capacity;
TZ_THREAD_LOCAL int32_t tz_group_freeing;

TzObject **tz_group_zombies;
size_t tz_group_zombie_count, tz_group_zombie_capacity;

#define TZ_RC_ZOMBIE (TZ_RC_ETERNAL + (INT64_C(1) << 59))

static void push(TzObject ***items, size_t *count, size_t *capacity, TzObject *leader) {
  if (*count == *capacity) {
    size_t grown = *capacity ? *capacity * 2 : 16;
    TzObject **copy = (TzObject **)tz_alloc(grown * sizeof(TzObject *));
    if (*count) {
      memcpy(copy, *items, *count * sizeof(TzObject *));
    }
    tz_free(*items);
    *items = copy;
    *capacity = grown;
  }
  (*items)[(*count)++] = leader;
}

#define CHAINS 16

static size_t drop_contents(TzObject *leader, uintptr_t *chains) {
  uintptr_t at = tz_group_link(leader)->next;
  if (tz_weak_count) {
    for (;;) {
      void *m = untagged(at);
      tz_weak_forget(m);
      if (m == leader) {
        break;
      }
      at = tz_group_link(m)->next;
    }
    at = tz_group_link(leader)->next;
  }
  uintptr_t last[CHAINS];
  size_t count = 0;
  for (;;) {
    void *m = untagged(at);
    uintptr_t following = tz_group_link(m)->next;
    __builtin_prefetch(tz_group_link(untagged(following)));
    if (chains) {
      size_t chain = count % CHAINS;
      if (count < CHAINS) {
        chains[chain] = at;
      } else {
        tz_group_link(untagged(last[chain]))->next = at;
      }
      last[chain] = at;
    }
    ++count;
    if (at & TZ_GROUP_ARRAY) {
      tz_arr_drop_member((TzArray *)m);
    } else {
      ((TzObject *)m)->desc->drop(m);
    }
    if (m == leader) {
      break;
    }
    at = following;
  }

  return count;
}

static void free_member(uintptr_t at) {
  if (at & TZ_GROUP_ARRAY) {
    --tz_live_arrays;
  } else {
    --tz_live_objects;
  }
  member_free(untagged(at));
}

static void free_memory(uintptr_t *chains, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    uintptr_t *chain = &chains[i % CHAINS];
    uintptr_t at = *chain;
    *chain = tz_group_link(untagged(at))->next;
    __builtin_prefetch(tz_group_link(untagged(*chain)), 1);
    free_member(at);
  }
}

static void free_ring(TzObject *leader) {
  uintptr_t at = tz_group_link(leader)->next;
  for (;;) {
    void *m = untagged(at);
    uintptr_t following = tz_group_link(m)->next;
    free_member(at);
    if (m == leader) {
      break;
    }
    at = following;
  }
}

TZ_NOINLINE void tz_group_free(TzObject *leader) {
  leader->rc = TZ_RC_DYING;
  if (tz_group_freeing) {
    push(&tz_group_dead, &tz_group_dead_count, &tz_group_dead_capacity, leader);
    return;
  }
  tz_group_freeing = 1;
  uintptr_t chains[CHAINS];
  for (;;) {
    free_memory(chains, drop_contents(leader, chains));
    if (!tz_group_dead_count) {
      break;
    }
    leader = tz_group_dead[--tz_group_dead_count];
  }
  tz_group_freeing = 0;
}

static void end_one(void *owner, void *ref) {
  (void)owner;
  if (!tz_rc_is_member(((TzObject *)ref)->rc)) {
    return;
  }
  TzObject *leader = leader_of(ref);
  if (!is_eternal(leader) || leader->rc >= TZ_RC_ZOMBIE - (INT64_C(1) << 58)) {
    return;
  }
  leader->rc = TZ_RC_ZOMBIE;
  push(&tz_group_zombies, &tz_group_zombie_count, &tz_group_zombie_capacity, leader);
  int32_t freeing = tz_group_freeing;
  tz_group_freeing = 1;
  drop_contents(leader, NULL);
  uintptr_t chains[CHAINS];
  while (tz_group_dead_count) {
    TzObject *dead = tz_group_dead[--tz_group_dead_count];
    free_memory(chains, drop_contents(dead, chains));
  }
  tz_group_freeing = freeing;
}

void tz_group_end_value(const TzType *type, void *value) {
  each_reference(type, value, NULL, end_one);
  tz_value_release(type, value);
}

typedef struct {
  const void *layout;
  void (*trace)(void *object, void *walk);
} TzGroupTrace;

typedef struct Walk Walk;
void tz_group_trace_ref(void *walk, void *ref, const char *field, uint32_t field_length);

const TzGroupTrace *tz_group_traces;
uint32_t tz_group_trace_count;

void tz_group_debug_init(const TzGroupTrace *traces, uint32_t count) {
  tz_group_debugging = 1;
  tz_group_traces = traces;
  tz_group_trace_count = count;
}

static void (*trace_of(const void *layout))(void *, void *) {
  for (uint32_t i = 0; i < tz_group_trace_count; ++i) {
    if (tz_group_traces[i].layout == layout) {
      return tz_group_traces[i].trace;
    }
  }
  return NULL;
}

typedef struct {
  const void **keys;
  const void **values;
  uint32_t capacity, used;
} AddressSet;

static uint32_t address_hash(const void *key) {
  return (uint32_t)(((uint64_t)(uintptr_t)key * UINT64_C(0x9E3779B97F4A7C15)) >> 32);
}

static uint32_t set_find(const AddressSet *set, const void *key) {
  uint32_t mask = set->capacity - 1;
  for (uint32_t i = address_hash(key) & mask;; i = (i + 1) & mask) {
    if (set->keys[i] == key || !set->keys[i]) {
      return i;
    }
  }
}

static void set_put(AddressSet *set, const void *key, const void *value) {
  if ((set->used + 1) * 2 > set->capacity) {
    AddressSet grown = {NULL, NULL, set->capacity ? set->capacity * 2 : 64, 0};
    grown.keys = (const void **)tz_alloc(grown.capacity * sizeof(void *));
    grown.values = (const void **)tz_alloc(grown.capacity * sizeof(void *));
    memset(grown.keys, 0, grown.capacity * sizeof(void *));
    for (uint32_t i = 0; i < set->capacity; ++i) {
      if (set->keys[i] && set->keys[i] != TOMBSTONE) {
        uint32_t at = set_find(&grown, set->keys[i]);
        grown.keys[at] = set->keys[i];
        grown.values[at] = set->values[i];
        ++grown.used;
      }
    }
    tz_free(set->keys);
    tz_free(set->values);
    *set = grown;
  }
  uint32_t at = set_find(set, key);
  if (!set->keys[at]) {
    ++set->used;
  }
  set->keys[at] = key;
  set->values[at] = value;
}

static const void *set_get(const AddressSet *set, const void *key) {
  if (!set->capacity) {
    return NULL;
  }
  uint32_t at = set_find(set, key);

  return set->keys[at] ? set->values[at] : NULL;
}

static void set_remove(AddressSet *set, const void *key) {
  if (!set->capacity) {
    return;
  }
  uint32_t at = set_find(set, key);
  if (set->keys[at]) {
    set->keys[at] = TOMBSTONE;
  }
}

static void set_free(AddressSet *set) {
  tz_free(set->keys);
  tz_free(set->values);
  memset(set, 0, sizeof *set);
}

static void place(char *out, size_t size, const TzFrame *at) {
  if (!at) {
    snprintf(out, size, "?");
    return;
  }
  const char *file = at->file;
  uint32_t length = at->file_length;
  for (uint32_t i = 0; i < at->file_length; ++i) {
    if (at->file[i] == '/' || at->file[i] == '\\') {
      file = at->file + i + 1;
      length = at->file_length - i - 1;
    }
  }
  snprintf(out, size, "%.*s:%u", (int)length, file, at->line);
}

static void debug_report(const char *text, size_t length) {
  tz_out_flush(1);
  tz_std_write_bytes(2, (const uint8_t *)text, length);
}

AddressSet tz_group_unstored;

void tz_group_note_unstored(const void *object, const TzFrame *made) {
  if (!object) {
    return;
  }
  group_lock();
  set_put(&tz_group_unstored, object, made);
  group_unlock();
}

void tz_group_forget_unstored(const void *object) {
  group_lock();
  set_remove(&tz_group_unstored, object);
  group_unlock();
}

TZ_NOINLINE void tz_group_check_unstored(const void *object) {
  group_lock();
  const TzFrame *made = (const TzFrame *)set_get(&tz_group_unstored, object);
  if (made) {
    set_remove(&tz_group_unstored, object);
  }
  group_unlock();
  if (!made) {
    return;
  }
  char where[256], text[512];
  place(where, sizeof where, made);
  char stored[256];
  place(stored, sizeof stored, tz_group_site ? tz_group_site->at : NULL);
  int length = snprintf(text, sizeof text,
                        "tz: debug check failed: the object made at %s, which the compiler found "
                        "is never stored, was stored at %s (docs/objects.md, section "
                        "11, layer 1)\n",
                        where, stored);
  debug_report(text, (size_t)length);
}

void tz_group_check_owned(const void *object, const TzFrame *at, const char *owner,
                          uint32_t owner_length) {
  const TzObject *o = (const TzObject *)object;
  if (!o || o->rc < 0 || o->rc == 1) {
    return;
  }
  char where[256];
  place(where, sizeof where, at);
  tz_panicf("debug check failed: the object in %.*s, made at %s, is held elsewhere when its owner "
            "goes (%lld references), which the compiler proved it is not (docs/objects.md, "
            "section 11, layer 2)",
            (int)owner_length, owner, where, (long long)o->rc);
}

typedef struct {
  const TzGroupSite *site;
  int64_t joined;
} SiteCount;

SiteCount tz_group_sites[64];
uint32_t tz_group_site_count;
char tz_group_reported[16][96];
uint32_t tz_group_reported_count;

struct Walk {
  TzObject *leader;
  AddressSet members;
  AddressSet reached;
  void **work;
  size_t work_count, work_capacity;
  int counting;
  const void *owner;
  uintptr_t owner_tag;
  char holders[16][96];
  int64_t holds[16];
  uint32_t holder_count;
  int untraced;
};

static void work_push(Walk *w, void *m) {
  if (w->work_count == w->work_capacity) {
    size_t grown = w->work_capacity ? w->work_capacity * 2 : 256;
    void **copy = (void **)tz_alloc(grown * sizeof(void *));
    if (w->work_count) {
      memcpy(copy, w->work, w->work_count * sizeof(void *));
    }
    tz_free(w->work);
    w->work = copy;
    w->work_capacity = grown;
  }
  w->work[w->work_count++] = m;
}

static void member_name(char *out, size_t size, void *m, uintptr_t tag);

void tz_group_trace_ref(void *walk, void *ref, const char *field, uint32_t field_length) {
  Walk *w = (Walk *)walk;
  TzObject *o = (TzObject *)ref;
  if (!o || !tz_rc_is_member(o->rc) || leader_of(o) != w->leader) {
    return;
  }
  if (!w->counting) {
    if (!set_get(&w->reached, o)) {
      set_put(&w->reached, o, o);
      work_push(w, o);
    }
    return;
  }
  if (set_get(&w->reached, o)) {
    return;
  }
  char name[64], holder[96];
  member_name(name, sizeof name, (void *)w->owner, w->owner_tag);
  snprintf(holder, sizeof holder, "%s.%.*s", name, (int)field_length, field);
  uint32_t k = 0;
  while (k < w->holder_count && strcmp(w->holders[k], holder) != 0) {
    ++k;
  }
  if (k == w->holder_count && k < 16) {
    memcpy(w->holders[k], holder, sizeof holder);
    w->holds[k] = 0;
    ++w->holder_count;
  }
  if (k < w->holder_count) {
    ++w->holds[k];
  }
}

static void trace_value(Walk *w, const TzType *type, void *value, const char *field) {
  if (type->kind == TZ_T_UNION) {
    const void *held = value;
    type = tz_resolve(type, &held);
    value = (void *)held;
  }
  switch (type->kind) {
  case TZ_T_ARRAY:
  case TZ_T_OBJECT:
  case TZ_T_UNKNOWN:
    tz_group_trace_ref(w, *(void **)value, field, (uint32_t)strlen(field));
    break;
  case TZ_T_FUNCTION:
    tz_group_trace_ref(w, ((TzFunction *)value)->env, field, (uint32_t)strlen(field));
    break;
  case TZ_T_OPTIONAL:
    trace_value(w, type->inner, value, field);
    break;
  case TZ_T_RESULT: {
    const TzResultType *result = (const TzResultType *)type;
    trace_value(w, type->inner, (char *)value + result->value_offset, field);
    trace_value(w, result->error, (char *)value + result->error_offset, field);
    break;
  }
  default:
    break;
  }
}

void tz_group_trace_value(void *walk, const TzType *type, void *value, const char *field) {
  trace_value((Walk *)walk, type, value, field);
}

static void trace_member(Walk *w, void *m, uintptr_t tag) {
  w->owner = m;
  w->owner_tag = tag & TZ_GROUP_ARRAY;
  if (tag & TZ_GROUP_ARRAY) {
    TzArray *a = (TzArray *)m;
    for (uint32_t i = 0; i < a->length; ++i) {
      trace_value(w, a->type, a->data + (size_t)i * a->type->size, "[i]");
    }
    return;
  }
  const TzLayout *layout = ((TzInstance *)m)->layout;
  if (layout->flags & TZ_LAYOUT_DICT) {
    TzMap *map = (TzMap *)m;
    const TzMapLayout *l = map->layout;
    uint32_t at = 0;
    for (char *e; (e = (char *)tz_map_next(map, &at));) {
      trace_value(w, l->key, e + l->key_offset, "key");
      if (l->value) {
        trace_value(w, l->value, e + l->value_offset, "value");
      }
    }
    return;
  }
  void (*trace)(void *, void *) = trace_of(layout);
  if (trace) {
    trace(m, w);
  } else {
    w->untraced = 1;
  }
}

static uintptr_t tag_of(Walk *w, void *m) {
  return (uintptr_t)set_get(&w->members, m) & TZ_GROUP_ARRAY;
}

static void walk_group(TzObject *leader) {
  Walk w;
  memset(&w, 0, sizeof w);
  w.leader = leader;
  int64_t total = 0;
  uintptr_t at = tz_group_link(leader)->next;
  for (;;) {
    void *m = untagged(at);
    ++total;
    set_put(&w.members, m, (const void *)((at & TZ_GROUP_ARRAY) | 2));
    if (listed(m)->outside > 0) {
      set_put(&w.reached, m, m);
      work_push(&w, m);
    }
    if (m == leader) {
      break;
    }
    at = tz_group_link(m)->next;
  }
  while (w.work_count && !w.untraced) {
    void *m = w.work[--w.work_count];
    trace_member(&w, m, tag_of(&w, m));
  }
  int64_t reached = 0;
  at = tz_group_link(leader)->next;
  for (;;) {
    void *m = untagged(at);
    if (set_get(&w.reached, m)) {
      ++reached;
    }
    if (m == leader) {
      break;
    }
    at = tz_group_link(m)->next;
  }
  int64_t lost = total - reached;
  if (!w.untraced && lost * 2 >= total) {
    w.counting = 1;
    at = tz_group_link(leader)->next;
    for (;;) {
      void *m = untagged(at);
      trace_member(&w, m, at);
      if (m == leader) {
        break;
      }
      at = tz_group_link(m)->next;
    }
    uint32_t best = 0;
    for (uint32_t k = 1; k < w.holder_count; ++k) {
      if (w.holds[k] > w.holds[best]) {
        best = k;
      }
    }
    const char *holder = w.holder_count ? w.holders[best] : "?";
    const TzGroupSite *site = NULL;
    const char *dot = strrchr(holder, '.');
    for (uint32_t i = 0; i < tz_group_site_count; ++i) {
      const TzGroupSite *s = tz_group_sites[i].site;
      if (dot && s->field && strlen(dot + 1) == s->field_length &&
          memcmp(dot + 1, s->field, s->field_length) == 0 &&
          (!site || tz_group_sites[i].joined > 0)) {
        site = s;
      }
    }
    char where[256] = "";
    if (site) {
      char at_text[200];
      place(at_text, sizeof at_text, site->at);
      snprintf(where, sizeof where, " at %s", at_text);
    }
    char names[8][64];
    int64_t counts[8];
    uint32_t kinds = 0;
    at = tz_group_link(leader)->next;
    for (;;) {
      void *m = untagged(at);
      char name[64];
      member_name(name, sizeof name, m, at);
      uint32_t k = 0;
      while (k < kinds && strcmp(names[k], name) != 0) {
        ++k;
      }
      if (k == kinds && k < 8) {
        memcpy(names[k], name, sizeof name);
        counts[k] = 0;
        ++kinds;
      }
      if (k < kinds) {
        ++counts[k];
      }
      if (m == leader) {
        break;
      }
      at = tz_group_link(m)->next;
    }
    uint32_t most = 0;
    for (uint32_t k = 1; k < kinds; ++k) {
      if (counts[k] > counts[most]) {
        most = k;
      }
    }
    char text[768];
    int length =
        snprintf(text, sizeof text,
                 "tz: group of %s: %lld of %lld objects cannot be reached, held by %s%s; "
                 "they stay until the whole group goes: make that reference a WeakRef, "
                 "or clear it when it is done (docs/objects.md, section 11)\n",
                 kinds ? names[most] : "?", (long long)lost, (long long)total, holder, where);
    uint32_t k = 0;
    while (k < tz_group_reported_count && strcmp(tz_group_reported[k], holder) != 0) {
      ++k;
    }
    if (k == tz_group_reported_count && k < 16) {
      snprintf(tz_group_reported[k], sizeof tz_group_reported[k], "%s", holder);
      ++tz_group_reported_count;
      debug_report(text, (size_t)length);
    }
  }
  set_free(&w.members);
  set_free(&w.reached);
  tz_free(w.work);
}

TZ_NOINLINE void tz_group_merged(TzObject *leader, int64_t joined) {
  if (tz_group_site) {
    uint32_t i = 0;
    while (i < tz_group_site_count && tz_group_sites[i].site != tz_group_site) {
      ++i;
    }
    if (i == tz_group_site_count && i < 64) {
      tz_group_sites[tz_group_site_count++] = (SiteCount){tz_group_site, 0};
    }
    if (i < tz_group_site_count) {
      tz_group_sites[i].joined += joined;
    }
  }
  int64_t size = listed(leader)->size, power = 4096;
  while (power <= size - joined) {
    power <<= 1;
  }
  if (size >= power) {
    walk_group(leader);
  }
}

static void member_name(char *out, size_t size, void *m, uintptr_t tag) {
  if (!m) {
    snprintf(out, size, "?");
    return;
  }
  if (tag & TZ_GROUP_ARRAY) {
    snprintf(out, size, "Array");
    return;
  }
  const TzLayout *layout = ((TzInstance *)m)->layout;
  if (layout->flags & TZ_LAYOUT_DICT) {
    uint32_t kind = ((const TzMapLayout *)layout)->kind;
    snprintf(out, size, "%s", kind == TZ_MAP_MAP ? "Map" : kind == TZ_MAP_SET ? "Set" : "Record");
    return;
  }
  if (layout->name) {
    snprintf(out, size, "%.*s", (int)layout->name_length, layout->name);
    return;
  }
  size_t at = (size_t)snprintf(out, size, "{");
  for (uint32_t i = 0; i < layout->count && at + 8 < size; ++i) {
    at += (size_t)snprintf(out + at, size - at, "%s %.*s", i ? "," : "",
                           (int)layout->fields[i].name_length, layout->fields[i].name);
  }
  if (at + 3 < size) {
    snprintf(out + at, size - at, " }");
  }
}

static void report(void) {
  typedef struct {
    char name[64];
    int64_t count;
  } Kind;
  for (Listed *l = tz_group_list.next; l != &tz_group_list; l = tz_group_list.next) {
    void *first = (char *)l + sizeof(Listed) + sizeof(TzGroupLink);
    (void)listed;
    TzObject *leader = leader_of(first);
    Kind kinds[8];
    size_t kind_count = 0;
    int64_t members = 0;
    uintptr_t at = tz_group_link(leader)->next;
    for (;;) {
      void *m = untagged(at);
      char name[64];
      member_name(name, sizeof name, m, at);
      size_t k = 0;
      while (k < kind_count && strcmp(kinds[k].name, name) != 0) {
        ++k;
      }
      if (k == kind_count && kind_count < 8) {
        memcpy(kinds[k].name, name, sizeof name);
        kinds[k].count = 0;
        ++kind_count;
      }
      if (k < kind_count) {
        ++kinds[k].count;
      }
      ++members;
      Listed *ml = (Listed *)((char *)m - sizeof(TzGroupLink) - sizeof(Listed));
      ml->prev->next = ml->next;
      ml->next->prev = ml->prev;
      ml->prev = ml->next = ml;
      if (m == leader) {
        break;
      }
      at = tz_group_link(m)->next;
    }
    char line[512];
    size_t len = (size_t)snprintf(line, sizeof line,
                                  "tz: a group of %lld object(s) leaked:", (long long)members);
    for (size_t k = 0; k < kind_count && len + 80 < sizeof line; ++k) {
      len += (size_t)snprintf(line + len, sizeof line - len, "%s %s (%lld)", k ? "," : "",
                              kinds[k].name, (long long)kinds[k].count);
    }
    if (len + 2 < sizeof line) {
      len += (size_t)snprintf(line + len, sizeof line - len, "\n");
    }
    tz_std_write_bytes(2, (const uint8_t *)line, len);
  }
}

void tz_group_shutdown(void) {
  while (tz_group_zombie_count) {
    free_ring(tz_group_zombies[--tz_group_zombie_count]);
  }
  tz_free(tz_group_zombies);
  tz_group_zombies = NULL;
  tz_group_zombie_capacity = 0;
  tz_free(tz_group_dead);
  tz_group_dead = NULL;
  tz_group_dead_capacity = 0;
  tz_free(tz_weak_slots);
  tz_weak_slots = NULL;
  tz_weak_capacity = tz_weak_used = 0;
  set_free(&tz_group_unstored);
  if (tz_group_listing) {
    report();
  }
}
