// Groups: how the memory of objects that may form cycles of references is
// freed without a collector that walks memory from time to time
// (docs/objects.md, section 11, layer 3).
//
// The compiler finds the types whose objects may form a cycle (see
// src/codegen/codegen_cycles.cpp) and makes their objects members of groups
// (TzGroupLink in tzrt.h); every other object is counted as before and pays
// nothing. A group is a ring of members, linked through `next`, with one
// count: the references from outside it, which its leader keeps. A reference
// that a member holds to a member of the same group is not counted.
//
// Storing a reference to a member into a member of another group merges the
// two: the smaller group joins the larger, its members get the larger's
// leader, the rings become one and the counts add up, less the reference just
// stored, which is inside now. Groups never split. When the count of a group
// reaches zero, nothing outside can reach its members, and they are freed
// together: first what each of them holds, skipping its references to the
// group, then the memory.
//
// A global that is never assigned again keeps its object for the whole run.
// Its group is eternal (tz_group_pin): references to it are not counted and
// merge nothing, so that the objects that point at the registry of an
// application do not all end up in its group; the references an eternal group
// holds to other groups are counted, as if from outside.
//
// A weak reference (WeakRef) is found from its object through a table, and
// becomes undefined exactly when the object dies: when its count reaches
// zero, or when its group is freed.

#include "tzrt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int64_t tz_live_objects; // closure.c
extern int64_t tz_live_arrays;  // array.c

// Set by tz_rt_init when TZ_CHECK_LEAKS is: every member is also listed, so
// that the groups left at the end can be reported, and counts the references
// to it from outside its group, for the walk of debug mode (see Debug mode
// below). Not static: see the rules in tzrt.h.
int32_t tz_group_listing;

typedef struct Listed {
  struct Listed *prev, *next;
  int64_t outside; // references to the member from outside its group
  int64_t size;    // of the group, at its leader
} Listed;

Listed tz_group_list = {&tz_group_list, &tz_group_list, 0, 0};

static inline Listed *listed(const void *member) {
  return (Listed *)((char *)member - sizeof(TzGroupLink) - sizeof(Listed));
}

// Debug mode (a program compiled with TZ_CHECK_LEAKS set): the place of the
// store being made, which a merge notes; and the walk of large groups.
int32_t tz_group_debugging;
const TzGroupSite *tz_group_site;
TZ_NOINLINE void tz_group_merged(TzObject *leader, int64_t joined);
TZ_NOINLINE void tz_group_check_unstored(const void *object);

static inline TzObject *leader_of(const void *member) {
  return (TzObject *)tz_group_link(member)->leader;
}

static inline void *untagged(uintptr_t next) { return (void *)(next & ~TZ_GROUP_ARRAY); }

static inline void *next_of(const void *member) { return untagged(tz_group_link(member)->next); }

static inline int is_eternal(const TzObject *leader) { return leader->rc >= TZ_RC_ETERNAL; }

// What lies in front of a member: its link, and in a program checked for
// leaks its place in the list before that.
static inline size_t front_size(void) {
  return sizeof(TzGroupLink) + (tz_group_listing ? sizeof(Listed) : 0);
}

//===----------------------------------------------------------------------===//
// Members
//===----------------------------------------------------------------------===//

// A member in a program checked for leaks, listed.
TZ_NOINLINE void *tz_group_alloc_listed(size_t size, uintptr_t tag) {
  Listed *l = (Listed *)tz_alloc(size + sizeof(Listed) + sizeof(TzGroupLink));
  l->prev = &tz_group_list;
  l->next = tz_group_list.next;
  tz_group_list.next->prev = l;
  tz_group_list.next = l;
  l->outside = 1;
  l->size = 1;
  char *member = (char *)l + sizeof(Listed) + sizeof(TzGroupLink);
  TzGroupLink *link = tz_group_link(member);
  link->leader = member;
  link->next = (uintptr_t)member | tag;
  ((TzObject *)member)->rc = TZ_RC_GROUP + 1;
  return member;
}

TZ_HOT void *tz_group_alloc_tagged(size_t size, uintptr_t tag) {
  if (tz_group_listing)
    return tz_group_alloc_listed(size, tag);
  char *member = (char *)tz_alloc(size + sizeof(TzGroupLink)) + sizeof(TzGroupLink);
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
    Listed *l = (Listed *)block;
    l->prev->next = l->next;
    l->next->prev = l->prev;
  }
  tz_free(block);
}

TZ_HOT void tz_group_obj_free(void *object) {
  if (((TzObject *)object)->rc >= 0) {
    if (tz_group_debugging)
      tz_group_forget_unstored(object);
    tz_obj_free(object);
  }
}

//===----------------------------------------------------------------------===//
// Counting
//===----------------------------------------------------------------------===//

TZ_HOT void tz_group_retain(void *member) {
  ++leader_of(member)->rc;
  if (tz_group_listing)
    ++listed(member)->outside;
}

TZ_HOT void tz_group_release(void *member) {
  TzObject *leader = leader_of(member);
  if (tz_group_listing)
    --listed(member)->outside;
  if (--leader->rc == TZ_RC_GROUP)
    tz_group_free(leader);
}

// Joins the groups of the leaders `a` and `b` and returns the leader of the
// whole. Walking both rings a step at a time finds the smaller one in as many
// steps as it has members; it then joins the other, so that a member changes
// its leader only when its group at least doubles, and building a group of n
// members takes O(n log n) steps at worst.
static TzObject *merge(TzObject *a, TzObject *b) {
  void *x = a, *y = b;
  for (;;) {
    x = next_of(x);
    y = next_of(y);
    if (x == a)
      break;
    if (y == b) {
      TzObject *t = a;
      a = b;
      b = t;
      break;
    }
  }
  void *m = a;
  do {
    tz_group_link(m)->leader = b;
    m = next_of(m);
  } while (m != a);
  uintptr_t t = tz_group_link(a)->next;
  tz_group_link(a)->next = tz_group_link(b)->next;
  tz_group_link(b)->next = t;
  b->rc += a->rc - TZ_RC_GROUP;
  a->rc = TZ_RC_MEMBER;
  if (tz_group_listing) {
    listed(b)->size += listed(a)->size;
    if (tz_group_debugging)
      tz_group_merged(b, listed(a)->size);
  }
  return b;
}

// tz_group_store of members of two groups; `ref` is the member stored.
TZ_NOINLINE void tz_group_link_groups(TzObject *owner, TzObject *leader, void *ref) {
  if (is_eternal(leader)) {
    --leader->rc; // a reference to an eternal group is not counted
    if (tz_group_listing)
      --listed(ref)->outside;
    return;
  }
  if (is_eternal(owner))
    return; // one from an eternal group is
  --merge(owner, leader)->rc;
  if (tz_group_listing)
    --listed(ref)->outside;
}

TZ_HOT void tz_group_store(void *owner, void *ref) {
  if (!ref)
    return;
  if (((TzObject *)ref)->rc >= 0) {
    if (tz_group_debugging)
      tz_group_check_unstored(ref);
    return;
  }
  if (((TzObject *)owner)->rc >= 0)
    return;
  TzObject *lo = leader_of(owner), *lr = leader_of(ref);
  if (lo == lr) {
    --lr->rc;
    if (tz_group_listing)
      --listed(ref)->outside;
  } else {
    tz_group_link_groups(lo, lr, ref);
  }
}

TZ_HOT void tz_group_unstore(void *owner, void *ref) {
  TzObject *lr = leader_of(ref);
  if (((TzObject *)owner)->rc < 0 && (leader_of(owner) == lr || is_eternal(lr)))
    return; // not counted
  if (tz_group_listing)
    --listed(ref)->outside;
  if (--lr->rc == TZ_RC_GROUP)
    tz_group_free(lr);
}

void tz_group_take(void *owner, void *ref) {
  if (((TzObject *)owner)->rc >= 0)
    return; // counted: the reference just moves
  TzObject *lr = leader_of(ref);
  if (leader_of(owner) == lr || is_eternal(lr)) {
    ++lr->rc; // not counted while the owner held it
    if (tz_group_listing)
      ++listed(ref)->outside;
  }
}

//===----------------------------------------------------------------------===//
// Values of any type
//===----------------------------------------------------------------------===//

// Calls `each(owner, ref)` for every reference to a counted object that a
// value of `type` at `value` holds (none for undefined).
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
    if (*(void **)value)
      each(owner, *(void **)value);
    break;
  case TZ_T_FUNCTION:
    if (((TzFunction *)value)->env)
      each(owner, ((TzFunction *)value)->env);
    break;
  case TZ_T_OPTIONAL: // undefined holds zero
    each_reference(type->inner, value, owner, each);
    break;
  case TZ_T_RESULT: { // the half not in use holds zero
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
  if (((TzObject *)ref)->rc < 0)
    tz_group_take(owner, ref);
}

static void pin_one(void *owner, void *ref) {
  (void)owner;
  if (((TzObject *)ref)->rc >= 0)
    return;
  TzObject *leader = leader_of(ref);
  if (leader->rc < TZ_RC_ETERNAL)
    leader->rc += TZ_RC_ETERNAL - TZ_RC_GROUP;
}

void tz_group_store_value(void *owner, const TzType *type, void *value) {
  if (((TzObject *)owner)->rc < 0 || tz_group_debugging)
    each_reference(type, value, owner, store_one);
}

void tz_group_take_value(void *owner, const TzType *type, void *value) {
  if (((TzObject *)owner)->rc < 0)
    each_reference(type, value, owner, take_one);
}

void tz_group_pin_value(const TzType *type, void *value) {
  each_reference(type, value, NULL, pin_one);
}

//===----------------------------------------------------------------------===//
// Weak references
//===----------------------------------------------------------------------===//

// A WeakRef: an object of the language whose fields the program does not
// see, the object it refers to (NULL once that died) and the next WeakRef to
// the same object. The table finds the first by the object. Must match
// CodeGen (src/codegen/codegen_cycles.cpp).
typedef struct TzWeak {
  int64_t rc;
  const TzLayout *layout;
  void *target;
  struct TzWeak *next;
} TzWeak;

// Open addressing by the object's address; a slot whose key is TOMBSTONE
// was freed. Not static: see the rules in tzrt.h.
typedef struct {
  void *key;
  TzWeak *first;
} WeakSlot;

WeakSlot *tz_weak_slots;
uint32_t tz_weak_capacity, tz_weak_used;
int64_t tz_weak_count; // live entries: objects that have weak references

#define TOMBSTONE ((void *)1)

static inline uint32_t weak_hash(const void *key) {
  uint64_t h = (uint64_t)(uintptr_t)key * UINT64_C(0x9E3779B97F4A7C15);
  return (uint32_t)(h >> 32);
}

static WeakSlot *weak_find(const void *key) {
  if (!tz_weak_capacity)
    return NULL;
  uint32_t mask = tz_weak_capacity - 1;
  for (uint32_t i = weak_hash(key) & mask;; i = (i + 1) & mask) {
    WeakSlot *s = &tz_weak_slots[i];
    if (s->key == key)
      return s;
    if (!s->key)
      return NULL;
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
    if (!slots[k].key || slots[k].key == TOMBSTONE)
      continue;
    uint32_t i = weak_hash(slots[k].key) & mask;
    while (tz_weak_slots[i].key)
      i = (i + 1) & mask;
    tz_weak_slots[i] = slots[k];
    ++tz_weak_used;
  }
  tz_free(slots);
}

// `new WeakRef(target)`: `target` is borrowed, and stays uncounted.
void *tz_weak_new(const TzLayout *layout, void *target) {
  TzWeak *w = (TzWeak *)tz_obj_alloc(sizeof(TzWeak));
  w->rc = 1;
  w->layout = layout;
  w->target = target;
  WeakSlot *s = weak_find(target);
  if (!s) {
    if ((tz_weak_used + 1) * 4 > tz_weak_capacity * 3)
      weak_grow();
    uint32_t mask = tz_weak_capacity - 1;
    uint32_t i = weak_hash(target) & mask;
    while (tz_weak_slots[i].key && tz_weak_slots[i].key != TOMBSTONE)
      i = (i + 1) & mask;
    s = &tz_weak_slots[i];
    if (!s->key)
      ++tz_weak_used;
    s->key = target;
    s->first = NULL;
    ++tz_weak_count;
  }
  w->next = s->first;
  s->first = w;
  return w;
}

// `w.deref()`: the object, counted, or NULL.
void *tz_weak_deref(void *weak) {
  void *target = ((TzWeak *)weak)->target;
  if (target)
    tz_ref_retain(target);
  return target;
}

// The object died: its weak references become undefined.
TZ_NOINLINE void tz_weak_forget(void *target) {
  WeakSlot *s = weak_find(target);
  if (!s)
    return;
  for (TzWeak *w = s->first; w; w = w->next)
    w->target = NULL;
  s->key = TOMBSTONE;
  s->first = NULL;
  --tz_weak_count;
}

// The drop function of a WeakRef: it leaves the list of its object.
void tz_weak_drop(void *object) {
  TzWeak *w = (TzWeak *)object;
  if (w->target) {
    WeakSlot *s = weak_find(w->target);
    TzWeak **at = &s->first;
    while (*at != w)
      at = &(*at)->next;
    *at = w->next;
    if (!s->first) {
      s->key = TOMBSTONE;
      --tz_weak_count;
    }
  }
  tz_obj_free(w);
}

//===----------------------------------------------------------------------===//
// Freeing groups
//===----------------------------------------------------------------------===//

// A group is freed on the spot, by the code that let go of it. docs/objects.md
// wants groups of more than ~10 000 members freed by a background thread; that
// needs lists of blocks freed by other threads in the pages of alloc.c, and is
// not done yet: tz_group_free is where it would hand a large group over.
//
// The groups whose count reached zero while another was being freed, freed
// one after another: a chain of groups takes no stack, however long. Not
// static: see the rules in tzrt.h.
TzObject **tz_group_dead;
size_t tz_group_dead_count, tz_group_dead_capacity;
int32_t tz_group_freeing;

// Eternal groups that the end of a program checked for leaks emptied: their
// memory goes last, as other groups may point at them without counting.
TzObject **tz_group_zombies;
size_t tz_group_zombie_count, tz_group_zombie_capacity;

#define TZ_RC_ZOMBIE (TZ_RC_ETERNAL + (INT64_C(1) << 59))

static void push(TzObject ***items, size_t *count, size_t *capacity, TzObject *leader) {
  if (*count == *capacity) {
    size_t grown = *capacity ? *capacity * 2 : 16;
    TzObject **copy = (TzObject **)tz_alloc(grown * sizeof(TzObject *));
    if (*count)
      memcpy(copy, *items, *count * sizeof(TzObject *));
    tz_free(*items);
    *items = copy;
    *capacity = grown;
  }
  (*items)[(*count)++] = leader;
}

// Walking a ring waits for each member to come from memory before it knows
// the next. So that free_memory fetches many at once, drop_contents turns the
// ring into this many chains, of every 16th member each, through the `next`
// of the members that it has passed: the drops look at the leaders only.
#define CHAINS 16

// What each member holds, while all of them are still there, so that the
// references among them are recognized and skipped; and its weak references
// go first, as the group is dead from now on. With `chains`, makes the
// chains and puts their first members there. Gives the number of members.
static size_t drop_contents(TzObject *leader, uintptr_t *chains) {
  uintptr_t at = tz_group_link(leader)->next;
  if (tz_weak_count) {
    for (;;) {
      void *m = untagged(at);
      tz_weak_forget(m);
      if (m == leader)
        break;
      at = tz_group_link(m)->next;
    }
    at = tz_group_link(leader)->next;
  }
  uintptr_t last[CHAINS]; // the members passed last, one per chain
  size_t count = 0;
  for (;;) {
    void *m = untagged(at);
    uintptr_t following = tz_group_link(m)->next;
    // The next member comes from memory while this one is dropped.
    __builtin_prefetch(tz_group_link(untagged(following)));
    if (chains) {
      size_t chain = count % CHAINS;
      if (count < CHAINS)
        chains[chain] = at;
      else
        tz_group_link(untagged(last[chain]))->next = at;
      last[chain] = at;
    }
    ++count;
    if (at & TZ_GROUP_ARRAY)
      tz_arr_drop_member((TzArray *)m);
    else
      ((TzObject *)m)->desc->drop(m);
    if (m == leader)
      break;
    at = following;
  }
  return count;
}

static void free_member(uintptr_t at) {
  if (at & TZ_GROUP_ARRAY)
    --tz_live_arrays;
  else
    --tz_live_objects;
  member_free(untagged(at));
}

// The memory of the `count` members, a member of each chain in turn: the
// order drop_contents passed them in. The `next` of the last member of a
// chain is stale, and only fetched ahead in vain.
static void free_memory(uintptr_t *chains, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    uintptr_t *chain = &chains[i % CHAINS];
    uintptr_t at = *chain;
    *chain = tz_group_link(untagged(at))->next;
    __builtin_prefetch(tz_group_link(untagged(*chain)), 1);
    free_member(at);
  }
}

// The same for a group whose contents went earlier, round its ring.
static void free_ring(TzObject *leader) {
  uintptr_t at = tz_group_link(leader)->next;
  for (;;) {
    void *m = untagged(at);
    uintptr_t following = tz_group_link(m)->next;
    free_member(at);
    if (m == leader)
      break;
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
    if (!tz_group_dead_count)
      break;
    leader = tz_group_dead[--tz_group_dead_count];
  }
  tz_group_freeing = 0;
}

// A global that held its value for good lets it go at the end of a program
// checked for leaks: an eternal group goes whatever its count, as the
// references to it were not counted; its memory stays until the others are
// gone (see tz_group_shutdown), and what still counts on it from then on
// counts in vain.
static void end_one(void *owner, void *ref) {
  (void)owner;
  if (((TzObject *)ref)->rc >= 0)
    return;
  TzObject *leader = leader_of(ref);
  if (!is_eternal(leader) || leader->rc >= TZ_RC_ZOMBIE - (INT64_C(1) << 58))
    return; // not eternal, or emptied already
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

//===----------------------------------------------------------------------===//
// Debug mode
//===----------------------------------------------------------------------===//

// A program compiled with TZ_CHECK_LEAKS set, as the tests are, checks at run
// time what the compiler proved about its memory (docs/objects.md, section
// 11), and walks a group from time to time to find the members that nothing
// outside the group reaches, which stay until the whole group goes.
//
// The walk needs to know the references in each member: arrays and
// dictionaries by their types, the other objects by a function the
// compiler makes for their layouts, which main hands over first.
typedef struct {
  const void *layout;
  void (*trace)(void *object, void *walk);
} TzGroupTrace;

// What the walk finds a reference to `ref` in a member, in its field `field`.
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
  for (uint32_t i = 0; i < tz_group_trace_count; ++i)
    if (tz_group_traces[i].layout == layout)
      return tz_group_traces[i].trace;
  return NULL;
}

// A set of addresses, open addressing; TOMBSTONE marks a freed slot.
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
  for (uint32_t i = address_hash(key) & mask;; i = (i + 1) & mask)
    if (set->keys[i] == key || !set->keys[i])
      return i;
}

static void set_put(AddressSet *set, const void *key, const void *value) {
  if ((set->used + 1) * 2 > set->capacity) {
    AddressSet grown = {NULL, NULL, set->capacity ? set->capacity * 2 : 64, 0};
    grown.keys = (const void **)tz_alloc(grown.capacity * sizeof(void *));
    grown.values = (const void **)tz_alloc(grown.capacity * sizeof(void *));
    memset(grown.keys, 0, grown.capacity * sizeof(void *));
    for (uint32_t i = 0; i < set->capacity; ++i)
      if (set->keys[i] && set->keys[i] != TOMBSTONE) {
        uint32_t at = set_find(&grown, set->keys[i]);
        grown.keys[at] = set->keys[i];
        grown.values[at] = set->values[i];
        ++grown.used;
      }
    tz_free(set->keys);
    tz_free(set->values);
    *set = grown;
  }
  uint32_t at = set_find(set, key);
  if (!set->keys[at])
    ++set->used;
  set->keys[at] = key;
  set->values[at] = value;
}

static const void *set_get(const AddressSet *set, const void *key) {
  if (!set->capacity)
    return NULL;
  uint32_t at = set_find(set, key);
  return set->keys[at] ? set->values[at] : NULL;
}

static void set_remove(AddressSet *set, const void *key) {
  if (!set->capacity)
    return;
  uint32_t at = set_find(set, key);
  if (set->keys[at])
    set->keys[at] = TOMBSTONE;
}

static void set_free(AddressSet *set) {
  tz_free(set->keys);
  tz_free(set->values);
  memset(set, 0, sizeof *set);
}

// The text of a place of the program, `file:line`.
static void place(char *out, size_t size, const TzFrame *at) {
  if (!at) {
    snprintf(out, size, "?");
    return;
  }
  // The file's name without its directories.
  const char *file = at->file;
  uint32_t length = at->file_length;
  for (uint32_t i = 0; i < at->file_length; ++i)
    if (at->file[i] == '/' || at->file[i] == '\\') {
      file = at->file + i + 1;
      length = at->file_length - i - 1;
    }
  snprintf(out, size, "%.*s:%u", (int)length, file, at->line);
}

static void debug_report(const char *text, size_t length) {
  tz_out_flush(1);
  tz_std_write_bytes(2, (const uint8_t *)text, length);
}

// Layer 1: the objects the compiler found are never stored where a
// reference can be kept from, by where they were made.
AddressSet tz_group_unstored;

void tz_group_note_unstored(const void *object, const TzFrame *made) {
  if (object)
    set_put(&tz_group_unstored, object, made);
}

void tz_group_forget_unstored(const void *object) { set_remove(&tz_group_unstored, object); }

// An object of a type that may form cycles took over a reference to an
// object that is no member: one made where the compiler proved that what it
// makes is never stored breaks the proof (and may leak, in a cycle).
TZ_NOINLINE void tz_group_check_unstored(const void *object) {
  const TzFrame *made = (const TzFrame *)set_get(&tz_group_unstored, object);
  if (!made)
    return;
  set_remove(&tz_group_unstored, object);
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

// Layer 2: an object that its owner made and holds alone, about to be let go
// by the owner, must have no other reference: what it holds refers to the
// owner without counting.
void tz_group_check_owned(const void *object, const TzFrame *at, const char *owner,
                          uint32_t owner_length) {
  const TzObject *o = (const TzObject *)object;
  if (!o || o->rc < 0 || o->rc == 1)
    return;
  char where[256];
  place(where, sizeof where, at);
  tz_panicf("debug check failed: the object in %.*s, made at %s, is held elsewhere when its owner "
            "goes (%lld references), which the compiler proved it is not (docs/objects.md, "
            "section 11, layer 2)",
            (int)owner_length, owner, where, (long long)o->rc);
}

// The sites of the stores that merged groups, with how many members each
// brought: the report names where the reference it blames is stored.
typedef struct {
  const TzGroupSite *site;
  int64_t joined;
} SiteCount;

SiteCount tz_group_sites[64];
uint32_t tz_group_site_count;
// The holders reported already.
char tz_group_reported[16][96];
uint32_t tz_group_reported_count;

struct Walk {
  TzObject *leader;
  AddressSet members; // the members, with their tags (+ 2)
  AddressSet reached; // the members reached
  void **work;
  size_t work_count, work_capacity;
  int counting; // the second pass: counts the references to unreached members
  const void *owner; // the member being traced
  uintptr_t owner_tag; // its tag: TZ_GROUP_ARRAY for an array
  // By holder (`Type.field`): how many references to unreached members.
  char holders[16][96];
  int64_t holds[16];
  uint32_t holder_count;
  int untraced; // a member whose references are not known
};

static void work_push(Walk *w, void *m) {
  if (w->work_count == w->work_capacity) {
    size_t grown = w->work_capacity ? w->work_capacity * 2 : 256;
    void **copy = (void **)tz_alloc(grown * sizeof(void *));
    if (w->work_count)
      memcpy(copy, w->work, w->work_count * sizeof(void *));
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
  if (!o || o->rc >= 0 || leader_of(o) != w->leader)
    return;
  if (!w->counting) {
    if (!set_get(&w->reached, o)) {
      set_put(&w->reached, o, o);
      work_push(w, o);
    }
    return;
  }
  if (set_get(&w->reached, o))
    return;
  char name[64], holder[96];
  member_name(name, sizeof name, (void *)w->owner, w->owner_tag);
  snprintf(holder, sizeof holder, "%s.%.*s", name, (int)field_length, field);
  uint32_t k = 0;
  while (k < w->holder_count && strcmp(w->holders[k], holder) != 0)
    ++k;
  if (k == w->holder_count && k < 16) {
    memcpy(w->holders[k], holder, sizeof holder);
    w->holds[k] = 0;
    ++w->holder_count;
  }
  if (k < w->holder_count)
    ++w->holds[k];
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

// The references a member holds, to tz_group_trace_ref.
static void trace_member(Walk *w, void *m, uintptr_t tag) {
  w->owner = m;
  w->owner_tag = tag & TZ_GROUP_ARRAY;
  if (tag & TZ_GROUP_ARRAY) {
    TzArray *a = (TzArray *)m;
    for (uint32_t i = 0; i < a->length; ++i)
      trace_value(w, a->type, a->data + (size_t)i * a->type->size, "[i]");
    return;
  }
  const TzLayout *layout = ((TzInstance *)m)->layout;
  if (layout->flags & TZ_LAYOUT_DICT) {
    TzMap *map = (TzMap *)m;
    const TzMapLayout *l = map->layout;
    uint32_t at = 0;
    for (char *e; (e = (char *)tz_map_next(map, &at));) {
      trace_value(w, l->key, e + l->key_offset, "key");
      if (l->value)
        trace_value(w, l->value, e + l->value_offset, "value");
    }
    return;
  }
  void (*trace)(void *, void *) = trace_of(layout);
  if (trace)
    trace(m, w);
  else
    w->untraced = 1;
}

// The tag of a member, which the pointer to it in the ring holds.
static uintptr_t tag_of(Walk *w, void *m) {
  return (uintptr_t)set_get(&w->members, m) & TZ_GROUP_ARRAY;
}

// Walks the group of `leader`: the members that the references from outside
// do not reach, through the references inside it, stay only because the
// group does. Reports where they are held, once for each holder.
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
    if (m == leader)
      break;
    at = tz_group_link(m)->next;
  }
  // Pass 1: what the roots reach.
  while (w.work_count && !w.untraced) {
    void *m = w.work[--w.work_count];
    trace_member(&w, m, tag_of(&w, m));
  }
  int64_t reached = 0;
  at = tz_group_link(leader)->next;
  for (;;) {
    void *m = untagged(at);
    if (set_get(&w.reached, m))
      ++reached;
    if (m == leader)
      break;
    at = tz_group_link(m)->next;
  }
  int64_t lost = total - reached;
  if (!w.untraced && lost * 2 >= total) {
    // Pass 2: who holds the members not reached.
    w.counting = 1;
    at = tz_group_link(leader)->next;
    for (;;) {
      void *m = untagged(at);
      trace_member(&w, m, at);
      if (m == leader)
        break;
      at = tz_group_link(m)->next;
    }
    uint32_t best = 0;
    for (uint32_t k = 1; k < w.holder_count; ++k)
      if (w.holds[k] > w.holds[best])
        best = k;
    const char *holder = w.holder_count ? w.holders[best] : "?";
    // The store of that field that joined the most.
    const TzGroupSite *site = NULL;
    const char *dot = strrchr(holder, '.');
    for (uint32_t i = 0; i < tz_group_site_count; ++i) {
      const TzGroupSite *s = tz_group_sites[i].site;
      if (dot && s->field && strlen(dot + 1) == s->field_length &&
          memcmp(dot + 1, s->field, s->field_length) == 0 &&
          (!site || tz_group_sites[i].joined > 0))
        site = s;
    }
    char where[256] = "";
    if (site) {
      char at_text[200];
      place(at_text, sizeof at_text, site->at);
      snprintf(where, sizeof where, " at %s", at_text);
    }
    // The type most members are of.
    char names[8][64];
    int64_t counts[8];
    uint32_t kinds = 0;
    at = tz_group_link(leader)->next;
    for (;;) {
      void *m = untagged(at);
      char name[64];
      member_name(name, sizeof name, m, at);
      uint32_t k = 0;
      while (k < kinds && strcmp(names[k], name) != 0)
        ++k;
      if (k == kinds && k < 8) {
        memcpy(names[k], name, sizeof name);
        counts[k] = 0;
        ++kinds;
      }
      if (k < kinds)
        ++counts[k];
      if (m == leader)
        break;
      at = tz_group_link(m)->next;
    }
    uint32_t most = 0;
    for (uint32_t k = 1; k < kinds; ++k)
      if (counts[k] > counts[most])
        most = k;
    char text[768];
    int length = snprintf(text, sizeof text,
                          "tz: group of %s: %lld of %lld objects cannot be reached, held by %s%s; "
                          "they stay until the whole group goes: make that reference a WeakRef, "
                          "or clear it when it is done (docs/objects.md, section 11)\n",
                          kinds ? names[most] : "?", (long long)lost, (long long)total, holder,
                          where);
    // Once for each holder.
    uint32_t k = 0;
    while (k < tz_group_reported_count && strcmp(tz_group_reported[k], holder) != 0)
      ++k;
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

// Debug mode, after two groups merged: notes the store's place, and walks a
// group whose size reached a power of two from 4096 on.
TZ_NOINLINE void tz_group_merged(TzObject *leader, int64_t joined) {
  if (tz_group_site) {
    uint32_t i = 0;
    while (i < tz_group_site_count && tz_group_sites[i].site != tz_group_site)
      ++i;
    if (i == tz_group_site_count && i < 64)
      tz_group_sites[tz_group_site_count++] = (SiteCount){tz_group_site, 0};
    if (i < tz_group_site_count)
      tz_group_sites[i].joined += joined;
  }
  // A walk each time the group reaches a power of two from 4096 on.
  int64_t size = listed(leader)->size, power = 4096;
  while (power <= size - joined)
    power <<= 1;
  if (size >= power)
    walk_group(leader);
}

//===----------------------------------------------------------------------===//
// The end of the program
//===----------------------------------------------------------------------===//

// The name the report gives a member.
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
  // An object of a literal: its keys.
  size_t at = (size_t)snprintf(out, size, "{");
  for (uint32_t i = 0; i < layout->count && at + 8 < size; ++i)
    at += (size_t)snprintf(out + at, size - at, "%s %.*s", i ? "," : "",
                           (int)layout->fields[i].name_length, layout->fields[i].name);
  if (at + 3 < size)
    snprintf(out + at, size - at, " }");
}

// Reports one line per group still alive: how many members it has, and of
// which types.
static void report(void) {
  // The leaders, and for each the names of its members with their counts.
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
      while (k < kind_count && strcmp(kinds[k].name, name) != 0)
        ++k;
      if (k == kind_count && kind_count < 8) {
        memcpy(kinds[k].name, name, sizeof name);
        kinds[k].count = 0;
        ++kind_count;
      }
      if (k < kind_count)
        ++kinds[k].count;
      ++members;
      // Unlisted as reported, so that the walk ends.
      Listed *ml = (Listed *)((char *)m - sizeof(TzGroupLink) - sizeof(Listed));
      ml->prev->next = ml->next;
      ml->next->prev = ml->prev;
      ml->prev = ml->next = ml;
      if (m == leader)
        break;
      at = tz_group_link(m)->next;
    }
    char line[512];
    size_t len = (size_t)snprintf(line, sizeof line, "tz: a group of %lld object(s) leaked:",
                                  (long long)members);
    for (size_t k = 0; k < kind_count && len + 80 < sizeof line; ++k)
      len += (size_t)snprintf(line + len, sizeof line - len, "%s %s (%lld)", k ? "," : "",
                              kinds[k].name, (long long)kinds[k].count);
    if (len + 2 < sizeof line)
      len += (size_t)snprintf(line + len, sizeof line - len, "\n");
    tz_std_write_bytes(2, (const uint8_t *)line, len);
  }
}

void tz_group_shutdown(void) {
  while (tz_group_zombie_count)
    free_ring(tz_group_zombies[--tz_group_zombie_count]);
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
  if (tz_group_listing)
    report();
}
