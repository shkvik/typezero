// Groups: cycles freed without a collector, and the counts of references.

#ifndef TZ_GROUP_H
#define TZ_GROUP_H

#include "runtime/core/closure/closure.h"
#include "runtime/core/tzrt/tzrt.h"

// Objects of the types the compiler finds may form cycles (docs/objects.md,
// section 11) are members of groups: an object of the language, an array, a
// dictionary, the environment of a closure or a box of a shared variable,
// each with a link of 16 bytes in front of it. A group has one count, of the
// references from outside it; those between its members are not counted.
// Storing a reference to a member into a member of another group merges the
// two groups. When nothing outside holds a member, the whole group is freed.
//
// A member's count is negative, which tells it from any other object: the
// count of a group lives in one member, its leader, and the others hold
// TZ_RC_MEMBER. The leader's count is TZ_RC_GROUP plus the references from
// outside; for an eternal group (one a global holds, which is never
// assigned again), TZ_RC_ETERNAL plus them: references to it are not counted
// and it merges with nothing. A group being freed has TZ_RC_DYING.
#define TZ_RC_MEMBER INT64_MIN
#define TZ_RC_DYING (-(INT64_C(1) << 62) - (INT64_C(1) << 60))
#define TZ_RC_GROUP (-(INT64_C(1) << 62))
#define TZ_RC_ETERNAL (-(INT64_C(1) << 61))

// Shared objects (docs/async.md, section 8). In the parallel runtime an
// object that other threads may reach, as soon as something they can reach
// holds it (tz_publish, parallel.c), is counted with atomic operations: its
// count is TZ_RC_SHARED plus its references, below zero, so that the fast
// paths, which count the others, leave it to the slow ones. The counts of
// groups are all below TZ_RC_SHARED: a count below it is a member's. A
// count from TZ_RC_FOREVER up is of an object never freed (on the stack, or
// one of the runtime's constants), which is far from zero whatever happens.
#define TZ_RC_SHARED (-(INT64_C(1) << 60))
#define TZ_RC_FOREVER (INT64_C(1) << 61)

static inline int tz_rc_is_member(int64_t rc) { return rc < TZ_RC_SHARED; }
static inline int tz_rc_is_shared(int64_t rc) { return rc < 0 && rc >= TZ_RC_SHARED; }

// One reference more, and one less, to an object that is never a member of
// a group (a string, a Json, a decimal...): tz_rc_dec says whether it was the
// last, and the caller frees the object. Plain counts, but for a shared
// object (only in the parallel runtime), whose count is negative and goes
// atomically: inline in tzrt_mt, through the functions below elsewhere
// (tzcrypto.lib is built once for both runtimes).
void tz_rc_shared_add(int64_t *rc, int64_t n);
int tz_rc_shared_dec(int64_t *rc);

static inline void tz_rc_add(int64_t *rc, int64_t n) {
  if (*rc < 0) {
#if TZ_PARALLEL && !defined(TZ_NO_THREAD_STATE)
    __atomic_fetch_add(rc, n, __ATOMIC_RELAXED);
#else
    tz_rc_shared_add(rc, n);
#endif
    return;
  }
  *rc += n;
}

static inline void tz_rc_inc(int64_t *rc) { tz_rc_add(rc, 1); }

static inline int tz_rc_dec(int64_t *rc) {
  if (*rc < 0) {
#if TZ_PARALLEL && !defined(TZ_NO_THREAD_STATE)
    if (__atomic_sub_fetch(rc, 1, __ATOMIC_ACQ_REL) != TZ_RC_SHARED) {
      return 0;
    }
    *rc = 0; // the last reference: the object is this thread's alone
    return 1;
#else
    return tz_rc_shared_dec(rc);
#endif
  }

  return --*rc == 0;
}

typedef struct TzGroupLink {
  void *leader;   // the member that holds the count; itself for the leader
  uintptr_t next; // the next member, round the group; TZ_GROUP_ARRAY tags an array
} TzGroupLink;

#define TZ_GROUP_ARRAY ((uintptr_t)1)

static inline TzGroupLink *tz_group_link(const void *member) {
  return (TzGroupLink *)((char *)member - sizeof(TzGroupLink));
}

// A new member of a group of its own, of `size` bytes, with a count of 1;
// the caller stores its descriptor and fields. Arrays come from
// tz_arr_new_group.
void *tz_group_alloc(int64_t size);
// The memory of a member with the TZ_GROUP_ARRAY tag or none, for the
// allocations above; not counted for the leak check.
void *tz_group_alloc_tagged(size_t size, uintptr_t tag);
// One more reference from outside to the group of `member`, or one less,
// which frees the group when it was the last. In the parallel runtime they
// also count a shared object (TZ_RC_SHARED), which has a descriptor that
// drops it: every object with a negative count comes here.
void tz_group_retain(void *member);
void tz_group_release(void *member);
// Frees the group of `leader`, whose count reached zero.
void tz_group_free(TzObject *leader);
// After `owner` took over a reference to `ref` into one of its fields
// (elements, entries...): if both are members, the reference is one inside
// a group now, and the groups merge if they were two.
void tz_group_store(void *owner, void *ref);
// `owner` lets go of its reference to the member `ref`.
void tz_group_unstore(void *owner, void *ref);
// The reference of `owner` to the member `ref` moves out of it to the code,
// which owns it then (pop, shift).
void tz_group_take(void *owner, void *ref);
// The end of the drop function of an object that may be a member: frees it,
// unless it is a member, which its group frees with the others.
void tz_group_obj_free(void *object);
// Publishing (publish.c): the group of `member` becomes published, and
// `each` gets every member of it, and whether it is an array; nothing if it
// was already.
void tz_group_publish(void *member, void (*each)(void *member, int32_t array));
int32_t tz_group_published(const void *member);
// The same for values of `type` at `value`, for every reference in them.
void tz_group_store_value(void *owner, const TzType *type, void *value);
void tz_group_take_value(void *owner, const TzType *type, void *value);
// Releases the value of `type` at `value` that `owner` held.
void tz_value_release_in(void *owner, const TzType *type, void *value);
// The groups of the references in a value that a global holds for good, and
// that value at the end of a program checked for leaks (see tz_group_end).
void tz_group_pin_value(const TzType *type, void *value);
void tz_group_end_value(const TzType *type, void *value);
// Before the leak check: frees what eternal groups left, and reports the
// groups still alive with the types of their members.
void tz_group_shutdown(void);

// Debug mode (group.c), in a program compiled with TZ_CHECK_LEAKS set: the
// place of the store into a member being made (tz_group_site), the objects
// the compiler proved are never stored, and those an owner holds alone.
struct TzFrame;
typedef struct TzGroupSite {
  const struct TzFrame *at;
  const char *field; // what the store writes, UTF-8
  uint32_t field_length;
} TzGroupSite;
extern int32_t tz_group_debugging;
extern TZ_THREAD_LOCAL const TzGroupSite *tz_group_site;
void tz_group_note_unstored(const void *object, const struct TzFrame *made);
void tz_group_forget_unstored(const void *object);
void tz_group_check_owned(const void *object, const struct TzFrame *at, const char *owner,
                          uint32_t owner_length);

static inline void tz_ref_retain(void *object) {
  TzObject *o = (TzObject *)object;
  if (o->rc >= 0) {
    ++o->rc;
  } else {
    tz_group_retain(o);
  }
}

static inline void tz_obj_retain(TzObject *o) {
  if (o) {
    tz_ref_retain(o);
  }
}

static inline void tz_obj_release(TzObject *o) {
  if (!o) {
    return;
  }
  if (o->rc < 0) {
    tz_group_release(o);
  } else if (--o->rc == 0) {
    o->desc->drop(o);
  }
}

static inline void tz_instance_release(TzInstance *o) {
  if (!o) {
    return;
  }
  if (o->rc < 0) {
    tz_group_release(o);
  } else if (--o->rc == 0) {
    o->layout->drop(o);
  }
}

// Weak references (group.c): a WeakRef is an object whose layout's drop
// function is tz_weak_drop; deref gives its object, counted, or NULL once
// that died. The drop function of an object that may have weak references
// forgets them first, when tz_weak_count says there are any.
extern int64_t tz_weak_count;
void *tz_weak_new(const TzLayout *layout, void *target);
void *tz_weak_deref(void *weak);
void tz_weak_forget(void *target);
void tz_weak_drop(void *object);

#endif
