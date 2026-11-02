#include "runtime/library/sync/sync/sync.h"
#include "runtime/core/closure/closure.h"
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
