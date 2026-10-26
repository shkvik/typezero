#ifndef TZ_GROUP_H
#define TZ_GROUP_H

#include "runtime/core/closure/closure.h"
#include "runtime/core/tzrt/tzrt.h"

#define TZ_RC_MEMBER INT64_MIN
#define TZ_RC_DYING (-(INT64_C(1) << 62) - (INT64_C(1) << 60))
#define TZ_RC_GROUP (-(INT64_C(1) << 62))
#define TZ_RC_ETERNAL (-(INT64_C(1) << 61))

#define TZ_RC_SHARED (-(INT64_C(1) << 60))
#define TZ_RC_FOREVER (INT64_C(1) << 61)

static inline int tz_rc_is_member(int64_t rc) { return rc < TZ_RC_SHARED; }
static inline int tz_rc_is_shared(int64_t rc) { return rc < 0 && rc >= TZ_RC_SHARED; }

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
    *rc = 0;
    return 1;
#else
    return tz_rc_shared_dec(rc);
#endif
  }

  return --*rc == 0;
}

typedef struct TzGroupLink {
  void *leader;
  uintptr_t next;
} TzGroupLink;

#define TZ_GROUP_ARRAY ((uintptr_t)1)

static inline TzGroupLink *tz_group_link(const void *member) {
  return (TzGroupLink *)((char *)member - sizeof(TzGroupLink));
}

void *tz_group_alloc(int64_t size);
void *tz_group_alloc_tagged(size_t size, uintptr_t tag);
void tz_group_retain(void *member);
void tz_group_release(void *member);
void tz_group_free(TzObject *leader);
void tz_group_store(void *owner, void *ref);
void tz_group_unstore(void *owner, void *ref);
void tz_group_take(void *owner, void *ref);
void tz_group_obj_free(void *object);
void tz_group_publish(void *member, void (*each)(void *member, int32_t array));
int32_t tz_group_published(const void *member);
void tz_group_store_value(void *owner, const TzType *type, void *value);
void tz_group_take_value(void *owner, const TzType *type, void *value);
void tz_value_release_in(void *owner, const TzType *type, void *value);
void tz_group_pin_value(const TzType *type, void *value);
void tz_group_end_value(const TzType *type, void *value);
void tz_group_shutdown(void);

struct TzFrame;
typedef struct TzGroupSite {
  const struct TzFrame *at;
  const char *field;
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

extern int64_t tz_weak_count;
void *tz_weak_new(const TzLayout *layout, void *target);
void *tz_weak_deref(void *weak);
void tz_weak_forget(void *target);
void tz_weak_drop(void *object);

#endif
