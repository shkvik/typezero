#include "runtime/library/sync/sync/sync.h"
#include "runtime/core/async/async.h"
#include "runtime/core/closure/closure.h"
#include "runtime/core/group/group.h"
#include "runtime/core/output/output.h"
#include "runtime/core/parallel/parallel.h"
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#error "TODO(linux): the lock of ConcurrentMap"
#endif

typedef struct TzSyncGuard {
  int64_t rc;
  const TzLayout *layout;
  SRWLOCK lock;
} TzSyncGuard;

static void guard_drop(void *object) { tz_obj_free(object); }

const TzLayout tz_sync_guard_layout = {guard_drop, NULL, 0, 0, "ConcurrentMap", 13, 0};

#define SYNC_NESTED                                                                                \
  "a ConcurrentMap is used inside the function given to update or getOrSet of a ConcurrentMap; "   \
  "call it before or after update or getOrSet"

static void enter(void) {
  if (tz_thread.sync_locks) {
    tz_panic(SYNC_NESTED, sizeof SYNC_NESTED - 1);
  }
}

void *tz_sync_guard(TzMaker make) {
  (void)make;
  TzSyncGuard *g = (TzSyncGuard *)tz_obj_alloc(sizeof(TzSyncGuard));
  memset(g, 0, sizeof *g);
  g->rc = 1;
  g->layout = &tz_sync_guard_layout;
  InitializeSRWLock(&g->lock);

  return g;
}

void tz_sync_read(void *guard, TzMaker make) {
  (void)make;
  enter();
  AcquireSRWLockShared(&((TzSyncGuard *)guard)->lock);
  ++tz_thread.sync_locks;
}

void tz_sync_read_done(void *guard, TzMaker make) {
  (void)make;
  --tz_thread.sync_locks;
  ReleaseSRWLockShared(&((TzSyncGuard *)guard)->lock);
}

void tz_sync_write(void *guard, TzMaker make) {
  (void)make;
  enter();
  AcquireSRWLockExclusive(&((TzSyncGuard *)guard)->lock);
  ++tz_thread.sync_locks;
}

void tz_sync_write_done(void *guard, TzMaker make) {
  (void)make;
  --tz_thread.sync_locks;
  ReleaseSRWLockExclusive(&((TzSyncGuard *)guard)->lock);
}

typedef struct TzSyncWaiter {
  int64_t rc;
  const TzLayout *layout;
  TzLock lock;
  TzPromise *promise;
  int32_t fired;
} TzSyncWaiter;

static void waiter_drop(void *object) {
  TzSyncWaiter *w = (TzSyncWaiter *)object;
  if (w->promise) {
    tz_obj_release((TzObject *)w->promise);
  }
  tz_obj_free(w);
}

const TzLayout tz_sync_waiter_layout = {waiter_drop, NULL, 0, 0, "Pool", 4, 0};

void *tz_sync_waiter(TzMaker make) {
  (void)make;
  TzSyncWaiter *w = (TzSyncWaiter *)tz_obj_alloc(sizeof(TzSyncWaiter));
  memset(w, 0, sizeof *w);
  w->rc = 1;
  w->layout = &tz_sync_waiter_layout;

  return w;
}

#if TZ_PARALLEL
void *tz_pool_port_handle(void);
int32_t tz_ahead_early(void);
void tz_trigger_hold(void);
void tz_work_ref_count(int32_t n);

typedef struct WaitLater {
  TzAfterPiece base;
  TzSyncWaiter *waiter;
  TzPromise *promise;
} WaitLater;

static void wait_later_drop(void *object) { tz_obj_free(object); }

static const TzObjectDesc wait_later_desc = {wait_later_drop};

static void wait_register(TzAfterPiece *a, int32_t pooled) {
  WaitLater *later = (WaitLater *)a;
  TzSyncWaiter *w = later->waiter;
  TzPromise *p = later->promise;
  tz_lock(&w->lock);
  int32_t fired = w->fired;
  if (!fired) {
    w->promise = p;
  }
  tz_unlock(&w->lock);
  tz_obj_release((TzObject *)w);
  if (fired) {
    if (pooled) {
      tz_trigger_hold();
    }
    tz_promise_succeed(p, NULL, 0);
    tz_obj_release((TzObject *)p);
    if (pooled) {
      tz_ahead_settle_held();
    }
  }
  tz_work_ref_count(-1);
}
#endif

void tz_sync_wait(TzPromise *p, void *waiter, TzMaker make) {
  (void)make;
  TzSyncWaiter *w = (TzSyncWaiter *)waiter;
#if TZ_PARALLEL
  if (tz_after_piece_wanted()) {
    WaitLater *later = (WaitLater *)tz_obj_alloc(sizeof(WaitLater));
    later->waiter = w;
    later->promise = p;
    tz_ref_retain(w);
    tz_ref_retain(p);
    tz_work_ref_count(1);
    tz_after_piece(&later->base, &wait_later_desc, wait_register);
    return;
  }
#endif
  tz_lock(&w->lock);
  if (w->fired) {
    tz_promise_succeed(p, NULL, 0);
  } else {
    tz_ref_retain(p);
    w->promise = p;
  }
  tz_unlock(&w->lock);
}

#if TZ_PARALLEL

static void wake_pooled(OVERLAPPED_ENTRY *entry) {
  TzPromise *p = (TzPromise *)entry->lpOverlapped;
  tz_trigger_hold();
  tz_promise_succeed(p, NULL, 0);
  tz_obj_release((TzObject *)p);
  tz_ahead_settle_held();
  tz_work_ref_count(-1);
}
#endif

void tz_sync_wake(void *waiter, TzMaker make) {
  (void)make;
  TzSyncWaiter *w = (TzSyncWaiter *)waiter;
  tz_lock(&w->lock);
  w->fired = 1;
  TzPromise *p = w->promise;
  w->promise = NULL;
  tz_unlock(&w->lock);
  if (!p) {
    return;
  }
#if TZ_PARALLEL
  HANDLE pool = tz_ahead_early() ? NULL : (HANDLE)tz_pool_port_handle();
  if (pool) {
    tz_work_ref_count(1);
    if (PostQueuedCompletionStatus(pool, 0, (ULONG_PTR)wake_pooled, (OVERLAPPED *)p)) {
      return;
    }
    tz_work_ref_count(-1);
  }
#endif
  tz_promise_succeed(p, NULL, 0);
  tz_obj_release((TzObject *)p);
}
