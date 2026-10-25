// The threads of the program: what each has, its turn in node's order, and locks.

#ifndef TZ_PARALLEL_H
#define TZ_PARALLEL_H

#include "runtime/core/alloc/alloc.h"
#include "runtime/core/tzrt/tzrt.h"

// The error in flight: the Error that `throw` (or a failing part of the
// runtime) handed over, until a `catch` takes it. A function that throws
// leaves it here and returns; its caller checks it after the call. The code
// of the program runs only while it is NULL.
//
// A function that may recurse throws RangeError when the stack is below
// tz_stack_limit at its start.
//
// Both are the thread's own, in one variable with the rest of what each
// thread has (TzThread), so that a function of the program finds them from
// one address of the thread (Lowering::thrownSlot). The first two fields
// must match Lowering.

typedef struct TzThread {
  void *thrown;
  char *stack_limit;
  // What the thread made and freed: the counts of the leak check
  // (TZ_CHECK_LEAKS), summed over the threads.
  int64_t live_objects, live_arrays, live_strings, live_decimals, live_json;
  // The parallel runtime (parallel.c): whether the thread holds the turn,
  // node's order, in which only one thread at a time goes on, the main one
  // but while it lends the turn to the piece of async code of another
  // thread; that piece, which this thread runs ahead of its turn; its
  // number, 0 for the main thread.
  int32_t turn;
  int32_t index;
  struct TzJob *job;
  // A thread of the pool, which never has the turn without a job.
  int32_t worker;
  // The locks of connections the thread holds (net.c): none may be held
  // while it waits for its turn.
  int32_t conn_locks;
  // The lock a function of the runtime took for the promise it gave, which
  // the await that follows lets go of once it waits (net.c, async.c).
  void *await_lock;
  // The microtasks of a piece that runs ahead of its event, in their order
  // (parallel.c): they go on here, or to the queue once it has the turn.
  struct TzReactor *local_head, *local_tail;
  // The caches of objects that the functions of a file keep for the next
  // calls (TZ_CACHE_*): each thread its own, as the formatters of ICU are
  // not to be used by two threads at once. NULL until the first use.
  void *caches[4];
  TzHeap heap;
} TzThread;

enum { TZ_CACHE_INTL = 0, TZ_CACHE_DATE_FORMAT = 1 };
#ifndef TZ_NO_THREAD_STATE
extern TZ_THREAD_LOCAL TzThread tz_thread;
#define tz_thrown (tz_thread.thrown)
#define tz_stack_limit (tz_thread.stack_limit)
#define tz_live_objects (tz_thread.live_objects)
#define tz_live_arrays (tz_thread.live_arrays)
#define tz_live_strings (tz_thread.live_strings)
#define tz_live_decimals (tz_thread.live_decimals)
#define tz_live_json (tz_thread.live_json)
#endif
// The sum of a count of TzThread (the int64_t at `offset`) over the threads
// of the program (parallel.c).
int64_t tz_thread_sum(size_t offset);
// Adds the thread that calls it to those tz_thread_sum counts.
void tz_thread_register(void);
// The threads of the program, by number: for the end of a program checked
// for leaks, when the others wait idle (parallel.c).
int32_t tz_thread_total(void);
TzThread *tz_thread_at(int32_t i);
// The number of the thread that calls it: 0 for the main one.
int32_t tz_thread_index(void);
// The pool of the parallel runtime, as TZ_THREADS asks (parallel.c).
void tz_parallel_init(void);
// Lets other threads run (parallel.c): for a thread that spins on a lock.
void tz_thread_yield(void);
static inline void tz_cpu_relax(void) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
  __builtin_ia32_pause();
#endif
}

// A lock that the thread that holds it may take again, for state of the
// runtime that threads share and that is rarely touched (the zone of the
// dates, what is made at the first use). Nothing in the runtime of one
// thread. Zero is a free lock.
typedef struct {
  int32_t owner; // tz_thread.index + 1 of the thread that holds it
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

// Node's order (parallel.c, docs/async.md, section 3). A thread of the pool
// runs a piece of async code ahead of its turn as long as the piece touches
// only what no other piece that may run at the same time writes; before the
// first thing that is not so, it waits for its turn: tz_order_enter(),
// which the program calls before such an access (Lowering, from the decisions
// of compiler/parallel/parallel_analysis/parallel_analysis.h), and TZ_ORDERED(), which the
// functions of the runtime that touch its shared state (the queues, the promises, the timers, the
// output...) begin with. On the main thread, and once the thread has its turn, both cost a test.
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
