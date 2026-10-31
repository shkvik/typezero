#ifndef TZ_PARALLEL_H
#define TZ_PARALLEL_H

#include "runtime/core/alloc/alloc.h"
#include "runtime/core/tzrt/tzrt.h"

typedef struct TzThread {
  void *failed;
  int64_t live_objects, live_arrays, live_strings, live_decimals, live_json;
  int32_t turn;
  int32_t index;
  struct TzJob *job;
  int32_t worker;
  int32_t conn_locks;
  void *await_lock;
  struct TzReactor *local_head, *local_tail;
  void *caches[4];
  TzHeap heap;
  int32_t awaited_call;
  int32_t send_count;
  void *sends[16];
} TzThread;

enum { TZ_CACHE_INTL = 0, TZ_CACHE_DATE_FORMAT = 1 };
#ifndef TZ_NO_THREAD_STATE
extern TZ_THREAD_LOCAL TzThread tz_thread;
#define tz_failed (tz_thread.failed)
#define tz_live_objects (tz_thread.live_objects)
#define tz_live_arrays (tz_thread.live_arrays)
#define tz_live_strings (tz_thread.live_strings)
#define tz_live_decimals (tz_thread.live_decimals)
#define tz_live_json (tz_thread.live_json)
#endif
int64_t tz_thread_sum(size_t offset);
void tz_thread_register(void);
int32_t tz_thread_total(void);
TzThread *tz_thread_at(int32_t i);
int32_t tz_thread_index(void);
void tz_parallel_init(void);
void tz_thread_yield(void);
void tz_call_awaited(void);
int32_t tz_take_awaited(void);
static inline void tz_cpu_relax(void) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
  __builtin_ia32_pause();
#endif
}

typedef struct {
  int32_t owner;
  int32_t depth;
} TzLock;

#if TZ_PARALLEL && !defined(TZ_NO_THREAD_STATE)
static inline void tz_lock(TzLock *l) {
  int32_t me = tz_thread.index + 1;
  if (__atomic_load_n(&l->owner, __ATOMIC_RELAXED) == me) {
    ++l->depth;
    return;
  }
  for (unsigned spin = 0;; ++spin) {
    int32_t none = 0;
    if (__atomic_compare_exchange_n(&l->owner, &none, me, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      break;
    }
    if (spin > 64) {
      tz_thread_yield();
    } else {
      tz_cpu_relax();
    }
  }
  l->depth = 1;
}

static inline void tz_unlock(TzLock *l) {
  if (--l->depth == 0) {
    __atomic_store_n(&l->owner, 0, __ATOMIC_RELEASE);
  }
}
#else
static inline void tz_lock(TzLock *l) { (void)l; }
static inline void tz_unlock(TzLock *l) { (void)l; }
#endif

#if TZ_PARALLEL && !defined(TZ_NO_THREAD_STATE)
TZ_NOINLINE void tz_order_wait(void);
static inline void tz_order_enter_inline(void) {
  if (!tz_thread.turn) {
    tz_order_wait();
  }
}
#define TZ_ORDERED() tz_order_enter_inline()
#else
#define TZ_ORDERED() ((void)0)
#endif
void tz_order_enter(void);

#endif
