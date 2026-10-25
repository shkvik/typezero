// The threads of the program (docs/async.md, section 8): what each has of
// its own (TzThread in tzrt.h), the list of them, for the counts summed over
// all of them, and in the parallel runtime the pool that runs pieces of
// async code ahead of their turn.
//
// tzrt, the runtime of a program that runs on one thread, has one TzThread,
// an ordinary variable. tzrt_mt (TZ_PARALLEL) has one for each thread.
//
// The turn. Node runs one piece of code at a time, in an order the program
// relies on: the main thread keeps that order, as the loop of async.c runs
// the microtasks, the timers and what I/O brings, one after another. A piece
// that comes after an await and may run at the same time as others
// (compiler/check/parallel/parallel.h) has a reactor marked `ahead`. When such
// a reactor waits in the microtask queue behind the one the main thread is
// about to run, the main thread hands it to an idle thread of the pool as a
// job. The thread resumes the coroutine right away, ahead of its turn, and runs
// it as long as the piece touches only what no other piece that may run at the
// same time writes: its own objects, data nobody changes. Before the first
// thing that keeps node's order (an access the compiler marked, a function of
// the runtime that touches its shared state: the queues, the promises, the
// output...) it waits for its turn (tz_order_wait).
//
// When the main thread comes to the reactor in the queue, it takes the job
// back if no thread has started it, and runs it itself. Otherwise it lends
// the thread its turn and waits until the piece is done: the rest of the
// piece runs on that thread, in order, as if the main thread ran it, and
// then the main thread goes on with the queue. So the program does what
// node would, in node's order, while the parts of pieces before their first
// ordered access run on all the cores at once.
//
// A job's state goes QUEUED -> RUNNING (a thread took it) -> WAITING (it
// waits for its turn) or GRANTED (the main thread lent it the turn first)
// -> DONE; or QUEUED -> TAKEN, when the main thread took it back. The
// threads wait on the state with WaitOnAddress.
//
// The threads of the pool wait on a completion port of their own: for the
// jobs, and for what the system says of the connections of servers (net.c).
// A thread that takes what came for a connection runs the piece that waits
// for it right there, as a job ahead of its event (see Events); a piece that
// touches nothing shared, the handling of a request mostly, never needs the
// main thread at all.
//
// TZ_THREADS sets how many threads run code, the main one among them: the
// number of processors by default; 1 runs everything on the main thread.

#include "runtime/core/tzrt/tzrt.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TZ_THREAD_LOCAL TzThread tz_thread;

int32_t tz_thread_index(void) { return tz_thread.index; }

#if TZ_PARALLEL

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#pragma comment(lib, "synchronization.lib")
#else
#error "TODO(linux): the threads of the parallel runtime"
#endif

#define MAX_THREADS 1024

// Not static: see the rules in tzrt.h. A thread adds itself once, and stays
// until the program ends: the threads of the pool never stop.
TzThread *tz_thread_list[MAX_THREADS];
int32_t tz_thread_count;

void tz_thread_register(void) {
  int32_t i = __atomic_fetch_add(&tz_thread_count, 1, __ATOMIC_ACQ_REL);
  if (i >= MAX_THREADS)
    tz_panic("too many threads", 16);
  tz_thread.index = i;
  __atomic_store_n(&tz_thread_list[i], &tz_thread, __ATOMIC_RELEASE);
}

int32_t tz_thread_total(void) { return __atomic_load_n(&tz_thread_count, __ATOMIC_ACQUIRE); }

TzThread *tz_thread_at(int32_t i) { return __atomic_load_n(&tz_thread_list[i], __ATOMIC_ACQUIRE); }

int64_t tz_thread_sum(size_t offset) {
  int32_t n = __atomic_load_n(&tz_thread_count, __ATOMIC_ACQUIRE);
  int64_t sum = 0;
  for (int32_t i = 0; i < n; ++i) {
    TzThread *t = __atomic_load_n(&tz_thread_list[i], __ATOMIC_ACQUIRE);
    if (t)
      sum += __atomic_load_n((const int64_t *)((const char *)t + offset), __ATOMIC_RELAXED);
  }
  return sum;
}

//===----------------------------------------------------------------------===//
// Jobs
//===----------------------------------------------------------------------===//

enum { JOB_QUEUED = 1, JOB_RUNNING, JOB_WAITING, JOB_GRANTED, JOB_DONE, JOB_TAKEN };

typedef struct TzJob {
  TzReactor *reactor;
  TzPromise *source; // what the reactor got from the queue, held by it
  struct TzJob *next;
  int32_t state;
  // Of an event: the main thread knows of it, in its list of those (see
  // Events); or it only says that `source` settled, and the main thread
  // queues its reactions.
  int32_t listed;
  int32_t forward;
  // Delivered: the microtask after its event that lends it the turn.
  TzReactor grant;
  uint32_t site;      // the await it goes on from (see Sites)
  int64_t started;    // when its thread started it (ticks)
  int64_t ahead;      // how long it ran ahead of its turn (ticks)
  // Run ahead of the event of I/O that settled its promise (see Events):
  // its thread wakes the poll phase once it waits for its turn.
  int32_t early;
  struct TzJob *later; // in the list of those
} TzJob;

static inline int64_t ticks(void) {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return now.QuadPart;
}

// A thread waits while the state is `seen`; spinning first, since the other
// side often answers within microseconds.
static void wait_state(int32_t *state, int32_t seen) {
  for (int spin = 0; spin < 4000; ++spin) {
    if (__atomic_load_n(state, __ATOMIC_ACQUIRE) != seen)
      return;
    YieldProcessor();
  }
  while (__atomic_load_n(state, __ATOMIC_ACQUIRE) == seen)
    WaitOnAddress(state, &seen, sizeof seen, INFINITE);
}

static void set_state(int32_t *state, int32_t value) {
  __atomic_store_n(state, value, __ATOMIC_RELEASE);
  WakeByAddressAll(state);
}

//===----------------------------------------------------------------------===//
// The pool
//===----------------------------------------------------------------------===//

// Whether pieces may go to the threads of the pool at all: TZ_THREADS asks
// for more than the main thread. Set at the start (tz_parallel_init).
int32_t tz_ahead_on;

// Not static: see the rules in tzrt.h. The port the threads wait on, where
// the jobs go (with the key POOL_JOB) and what comes for the connections
// of servers (with the function that takes it as the key); the lock for
// starting threads.
HANDLE tz_pool_port;
#define POOL_JOB ((ULONG_PTR)1)
SRWLOCK tz_pool_lock = SRWLOCK_INIT;
int32_t tz_pool_queued;  // jobs posted, not taken yet
int32_t tz_pool_idle;    // threads waiting on the port
int32_t tz_pool_started; // threads started
int32_t tz_pool_running; // threads running a job (atomic)
int32_t tz_pool_size = -1; // the threads it may have: TZ_THREADS - 1, and the spares
int32_t tz_pool_base;      // TZ_THREADS - 1
int64_t tz_pool_spare_at;  // when the pool last looked for a spare (ticks)

// The jobs the main thread may still hand out: threads idle or not started
// yet, less the jobs waiting; read without the lock, as an estimate.
static int32_t pool_room(void) {
  return tz_pool_size - __atomic_load_n(&tz_pool_running, __ATOMIC_RELAXED) -
         __atomic_load_n(&tz_pool_queued, __ATOMIC_RELAXED);
}

void tz_heap_collect(void);

// Each await after which pieces may run ahead of their turn (see Sites).
typedef struct {
  uint32_t runs;
  uint32_t probe;
  uint32_t counted; // the reactors kept, and those timed on the main thread
  uint32_t every; // a short site gives one of this many to a thread
  double average; // ticks
} Site;
extern Site *tz_ahead_sites;
extern uint32_t tz_ahead_site_count;

// With TZ_PARALLEL_STATS set, the program tells at its end how many pieces
// went to the pool, how many ran there and how many the main thread took
// back: for the developers of tz, and benchmarks.
int64_t tz_pool_stat_dispatched, tz_pool_stat_ran, tz_pool_stat_taken, tz_pool_stat_waited;

// With TZ_PARALLEL_STRESS set, the main thread never takes a piece back: it
// waits for a thread of the pool to start it, so that every piece that may
// run ahead does, as tests then check (tests/CMakeLists.txt).
int32_t tz_pool_stress;

// With TZ_PARALLEL_WATCHDOG=n, a thread prints after n seconds what the
// threads wait for: for the developers of tz, who look for a deadlock.
TzJob *tz_pool_main_waits; // the job the main thread waits for
extern int32_t tz_group_lock_owner;
extern int32_t tz_str_flatten_lock;
const char *tz_thread_where[64];

static DWORD WINAPI watchdog(void *arg) {
  Sleep((DWORD)(intptr_t)arg * 1000);
  TzJob *j = tz_pool_main_waits;
  fprintf(stderr, "tz watchdog: main waits for job %p (state %d), group lock %d, flatten lock %d, "
                  "queued %d idle %d running %d started %d\n",
          (void *)j, j ? j->state : -1, tz_group_lock_owner, tz_str_flatten_lock, tz_pool_queued,
          tz_pool_idle, tz_pool_running, tz_pool_started);
  for (int i = 0; i < 64; ++i)
    if (tz_thread_where[i])
      fprintf(stderr, "  thread %d: %s\n", i, tz_thread_where[i]);
  for (int32_t i = 1; i < tz_thread_total(); ++i) {
    TzThread *t = tz_thread_at(i);
    TzJob *job = t ? t->job : NULL;
    if (job)
      fprintf(stderr, "  thread %d runs job %p: state %d, early %d, reactor %p\n", i, (void *)job,
              job->state, job->early, (void *)job->reactor);
  }
  extern TzJob *tz_ahead_early_head;
  for (TzJob *job = tz_ahead_early_head; job; job = job->later)
    fprintf(stderr, "  early job %p: state %d\n", (void *)job, job->state);
  extern TzReactor *tz_micro_head;
  fprintf(stderr, "  microtask queue %s; pending I/O %d\n", tz_micro_head ? "has reactors" : "empty",
          tz_io_alive());
  fflush(stderr);
  return 0;
}

int32_t tz_pool_stat_where;
struct {
  uintptr_t at;
  int64_t count;
} tz_pool_stop_at[8];

static void print_stats(void) {
  for (int i = 0; i < 8 && tz_pool_stop_at[i].at; ++i)
    fprintf(stderr, "  stopped ahead at +0x%llx: %lld times\n",
            (unsigned long long)tz_pool_stop_at[i].at, (long long)tz_pool_stop_at[i].count);
  fprintf(stderr, "tz: %lld pieces handed out, %lld ran ahead, %lld taken back, %lld waited for\n",
          (long long)tz_pool_stat_dispatched, (long long)tz_pool_stat_ran,
          (long long)tz_pool_stat_taken, (long long)tz_pool_stat_waited);
  for (uint32_t i = 0; i < tz_ahead_site_count && i < 8; ++i)
    if (tz_ahead_sites[i].runs)
      fprintf(stderr, "  site %u: %u runs, %.0f ticks on average\n", i + 1,
              tz_ahead_sites[i].runs, tz_ahead_sites[i].average);
}

extern TzReactor *tz_micro_head, *tz_micro_tail; // async.c

// A piece ahead of its event: then the microtasks it queued meanwhile, in
// order, here while it has no turn (tz_ahead_local); once it has, the queue
// has them (tz_order_wait). It lets go of what the queue would have.
static void run_early(TzJob *job) {
  TzReactor *r = job->reactor;
  TzPromise *source = job->source;
  TzObject *owner = r->owner;
  r->run(r, source);
  if (source)
    tz_obj_release((TzObject *)source);
  tz_obj_release(owner);
  while (tz_thread.local_head) {
    TzReactor *next = tz_thread.local_head;
    tz_thread.local_head = next->next;
    if (!tz_thread.local_head)
      tz_thread.local_tail = NULL;
    TzPromise *got = next->source;
    TzObject *of = next->owner;
    next->source = NULL;
    next->run(next, got);
    if (got)
      tz_obj_release((TzObject *)got);
    tz_obj_release(of);
  }
}

// Gives whether the job waited for its turn.
static int run_job(TzJob *job) {
  TzReactor *r = job->reactor;
  job->started = ticks();
  __atomic_store_n(&tz_thread.job, job, __ATOMIC_RELEASE);
  tz_thread.turn = 0;
  job->ahead = -1;
  __atomic_fetch_add(&tz_pool_stat_ran, 1, __ATOMIC_RELAXED);
  if (job->early)
    run_early(job);
  else
    r->run(r, job->source);
  if (job->ahead < 0) // it never waited: all of it ran ahead
    job->ahead = ticks() - job->started;
  tz_thread.turn = 0;
  __atomic_store_n(&tz_thread.job, NULL, __ATOMIC_RELEASE);
  int32_t early = job->early;
  if (early && !job->listed) {
    // Ahead of an event the main thread never heard of, and none of it
    // needed the turn: nothing is left to do with it.
    free(job);
    return 0;
  }
  set_state(&job->state, JOB_DONE);
  if (early)
    tz_io_wake_loop();
  return 1;
}

static DWORD WINAPI worker_main(void *arg) {
  (void)arg;
  tz_thread_register();
  tz_thread_stack_init();
  tz_thread.turn = 0;
  tz_thread.worker = 1;
  for (;;) {
    // What other threads freed of this one's memory goes back first.
    if (__atomic_load_n(&tz_thread.heap.remote, __ATOMIC_RELAXED))
      tz_heap_collect();
    OVERLAPPED_ENTRY entry;
    ULONG count = 0;
    __atomic_fetch_add(&tz_pool_idle, 1, __ATOMIC_RELAXED);
    BOOL ok = GetQueuedCompletionStatusEx(tz_pool_port, &entry, 1, &count, INFINITE, FALSE);
    __atomic_fetch_sub(&tz_pool_idle, 1, __ATOMIC_RELAXED);
    if (!ok || count == 0)
      continue;
    __atomic_fetch_add(&tz_pool_running, 1, __ATOMIC_RELAXED);
    if (entry.lpCompletionKey == POOL_JOB) {
      TzJob *job = (TzJob *)entry.lpOverlapped;
      __atomic_fetch_sub(&tz_pool_queued, 1, __ATOMIC_RELAXED);
      int32_t queued = JOB_QUEUED;
      if (__atomic_compare_exchange_n(&job->state, &queued, JOB_RUNNING, 0, __ATOMIC_ACQ_REL,
                                      __ATOMIC_ACQUIRE)) {
        WakeByAddressAll(&job->state); // the main thread may wait for it to start
        run_job(job);
      } else {
        free(job); // the main thread took it back: it is done with it
      }
    } else {
      // What the system says of a connection (net.c).
      ((void (*)(OVERLAPPED_ENTRY *))entry.lpCompletionKey)(&entry);
    }
    __atomic_fetch_sub(&tz_pool_running, 1, __ATOMIC_RELAXED);
  }
  return 0;
}

static int32_t pool_size(void) {
  if (tz_pool_size >= 0)
    return tz_pool_size;
  int32_t threads = 0;
  const char *value = getenv("TZ_THREADS");
  if (value && *value)
    threads = atoi(value);
  if (threads <= 0) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    threads = info.dwNumberOfProcessors > 0 ? (int32_t)info.dwNumberOfProcessors : 1;
  }
  if (threads > MAX_THREADS / 2)
    threads = MAX_THREADS / 2;
  tz_pool_size = threads - 1;
  tz_pool_base = tz_pool_size;
  if (tz_pool_size > 0) {
    // As many threads at once as want to run: spares too (see grow_pool).
    tz_pool_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, MAX_THREADS);
    if (!tz_pool_port)
      tz_pool_size = tz_pool_base = 0;
  }
  if ((tz_pool_stat_where = getenv("TZ_PARALLEL_STATS") != NULL))
    atexit(print_stats);
  tz_pool_stress = getenv("TZ_PARALLEL_STRESS") != NULL;
  if (getenv("TZ_PARALLEL_WATCHDOG")) {
    HANDLE t = CreateThread(NULL, 0, watchdog,
                            (void *)(intptr_t)atoi(getenv("TZ_PARALLEL_WATCHDOG")), 0, NULL);
    if (t)
      CloseHandle(t);
  }
  return tz_pool_size;
}

// Starts one more thread, up to `limit`.
static void start_one(int32_t limit) {
  AcquireSRWLockExclusive(&tz_pool_lock);
  if (tz_pool_started < limit) {
    // As much stack as the main thread has (see the linker's /stack).
    HANDLE thread = CreateThread(NULL, 8 * 1024 * 1024, worker_main, NULL,
                                 STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (thread) {
      CloseHandle(thread);
      __atomic_store_n(&tz_pool_started, tz_pool_started + 1, __ATOMIC_RELAXED);
    }
  }
  ReleaseSRWLockExclusive(&tz_pool_lock);
}

// Threads start as jobs come, up to the size of the pool.
static void start_thread(void) {
  if (__atomic_load_n(&tz_pool_started, __ATOMIC_RELAXED) >= tz_pool_size ||
      __atomic_load_n(&tz_pool_idle, __ATOMIC_RELAXED) >=
          __atomic_load_n(&tz_pool_queued, __ATOMIC_RELAXED))
    return;
  start_one(tz_pool_size);
}

static void push_job(TzJob *job) {
  job->next = NULL;
  __atomic_fetch_add(&tz_pool_queued, 1, __ATOMIC_RELAXED);
  start_thread();
  PostQueuedCompletionStatus(tz_pool_port, 0, POOL_JOB, (OVERLAPPED *)job);
}

// net.c: the port the connections of a server wait on, NULL when the pool
// has no threads; with the first of them, all the threads of the pool
// start, which take what comes for them.
void *tz_pool_port_handle(void) {
  if (!tz_ahead_on)
    return NULL;
  while (__atomic_load_n(&tz_pool_started, __ATOMIC_RELAXED) < tz_pool_base)
    start_one(tz_pool_base);
  return tz_pool_port;
}

//===----------------------------------------------------------------------===//
// The turn
//===----------------------------------------------------------------------===//

void tz_parallel_init(void) { tz_ahead_on = pool_size() > 0; }

//===----------------------------------------------------------------------===//
// Sites
//===----------------------------------------------------------------------===//

// Handing a piece to another thread costs microseconds; a piece that runs
// ahead for less does better on the main thread. Each await after which
// pieces may run ahead is a site (CodeGen numbers them from 1, and the
// reactor's `ahead` is the number): it learns how long its pieces run before
// their first ordered access, an average that leans on the last ones. A site
// whose pieces are short gives one to a thread now and then only, to see
// whether they grew; one not known yet gives them all.

// Not static: see the rules in tzrt.h. Grown by the main thread as it meets
// sites; `tz_ahead_worth` in ticks.
Site *tz_ahead_sites;
uint32_t tz_ahead_site_count;
double tz_ahead_worth;

static Site *site_of(uint32_t n) {
  if (n > tz_ahead_site_count) {
    uint32_t count = tz_ahead_site_count ? tz_ahead_site_count : 64;
    while (count < n)
      count *= 2;
    Site *grown = (Site *)realloc(tz_ahead_sites, count * sizeof(Site));
    if (!grown)
      return NULL;
    memset(grown + tz_ahead_site_count, 0, (count - tz_ahead_site_count) * sizeof(Site));
    tz_ahead_sites = grown;
    tz_ahead_site_count = count;
  }
  return &tz_ahead_sites[n - 1];
}

static void learn(uint32_t n, int64_t spent) {
  Site *s = site_of(n);
  if (!s)
    return;
  s->average = s->runs ? s->average * 0.75 + (double)spent * 0.25 : (double)spent;
  ++s->runs;
  if (s->average >= tz_ahead_worth)
    s->every = 0; // long again: probed often once it is short
}

static int is_short(const Site *s) { return s->runs >= 4 && s->average < tz_ahead_worth; }

// Each reactor is looked at once (EXAMINED, below); a short site sends one
// of every 256 to a thread at first, and each time it is still short, half
// as many, down to one of 65 536: a probe costs the main thread the wait for
// a thread to wake up.
static int worth_a_thread(Site *s) {
  if (tz_pool_stress || !is_short(s))
    return 1;
  if (s->every < 256)
    s->every = 256;
  if (++s->probe < s->every)
    return 0;
  s->probe = 0;
  if (s->every < 65536)
    s->every *= 2;
  return 1;
}

// How many reactors past the head the main thread looks at for ones to hand
// out: those further back wait until the queue comes closer.
#define AHEAD_WINDOW 64
// The numbers of sites are below this; a job's address is above it.
#define MAX_SITE ((uintptr_t)1 << 20)
// Added to the number of the site of a reactor looked at and left to the
// main thread: it is not looked at again while it waits.
#define EXAMINED ((uintptr_t)1 << 19)

// Preemption (docs/async.md, section 8). A piece runs on its thread to its
// first ordered access, however long; when every thread of the pool has
// been on one piece for more than 10 ms and more pieces could go, the pool
// takes one thread more, and the system shares the cores among them, so
// that short pieces do not wait for long ones. Up to as many spares as the
// pool has threads; they stay, as the others do.
#define SPARE_AFTER 10e-3 // seconds

static int grow_pool(void) {
  if (tz_pool_base <= 0 || tz_pool_size >= 2 * tz_pool_base)
    return 0;
  int64_t now = ticks();
  LARGE_INTEGER frequency;
  QueryPerformanceFrequency(&frequency);
  int64_t after = (int64_t)((double)frequency.QuadPart * SPARE_AFTER);
  // Not more often than that either.
  if (now - tz_pool_spare_at < after)
    return 0;
  tz_pool_spare_at = now;
  int32_t n = tz_thread_total();
  for (int32_t i = 1; i < n; ++i) {
    TzThread *t = tz_thread_at(i);
    TzJob *job = t ? __atomic_load_n(&t->job, __ATOMIC_ACQUIRE) : NULL;
    if (!job)
      return 0; // a thread is free, or about to be
    int64_t started = __atomic_load_n(&job->started, __ATOMIC_RELAXED);
    if (__atomic_load_n(&job->state, __ATOMIC_ACQUIRE) != JOB_RUNNING || now - started < after)
      return 0;
  }
  __atomic_fetch_add(&tz_pool_size, 1, __ATOMIC_RELAXED);
  return 1;
}

// The last reactor of the queue looked at, which those after it follow;
// NULL once it left the queue (tz_ahead_popped), when the look starts again
// after the head. So each reactor is looked at about once. Not static: see
// the rules in tzrt.h.
TzReactor *tz_ahead_last;

void tz_ahead_popped(TzReactor *r) {
  if (r == tz_ahead_last)
    tz_ahead_last = NULL;
}

void tz_ahead_dispatch(TzReactor *head) {
  if (pool_room() <= 0 && !grow_pool())
    return;
  if (tz_ahead_worth == 0) {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    tz_ahead_worth = (double)frequency.QuadPart * 30e-6; // 30 microseconds
  }
  int scanned = 0;
  TzReactor *first = tz_ahead_last ? tz_ahead_last->next : head->next;
  for (TzReactor *r = first; r && scanned < AHEAD_WINDOW; r = r->next, ++scanned) {
    uintptr_t site = r->ahead;
    if (site != 0 && site < EXAMINED) {
      Site *s = site_of((uint32_t)site);
      if (!s)
        return;
      if (!worth_a_thread(s)) {
        r->ahead = site | EXAMINED;
      } else {
        // Without room it waits for the next look, or a spare thread.
        if (pool_room() <= 0 && !grow_pool())
          return;
        TzJob *job = (TzJob *)malloc(sizeof(TzJob));
        if (!job)
          return;
        job->reactor = r;
        job->source = r->source;
        job->state = JOB_QUEUED;
        job->site = (uint32_t)site;
        job->early = 0;
        job->listed = 0;
        job->forward = 0;
        job->later = NULL;
        r->source = NULL;
        r->ahead = (uintptr_t)job;
        ++tz_pool_stat_dispatched;
        push_job(job);
      }
    }
    tz_ahead_last = r;
  }
}

// What a reactor of `site` gets as its `ahead` at the await: for most of
// the reactors of a site known to be short, the number marked EXAMINED, so
// that the main thread does not hand them out; a thread of the pool that
// takes their event still runs them where it is (see Events).
int32_t tz_ahead_keep(int32_t site) {
  Site *s = site_of((uint32_t)site);
  if (!s || tz_pool_stress || !is_short(s))
    return site;
  return (++s->counted & 63) == 0 ? site : (int32_t)(site | EXAMINED);
}

//===----------------------------------------------------------------------===//
// Events
//===----------------------------------------------------------------------===//

// An event of I/O (a read of a file done) is delivered in the poll phase,
// and the piece that awaited its promise runs as a microtask after it. When
// is up to the system in node, so the event may come later: the piece goes
// to a thread at once, runs ahead while the main thread goes on with other
// events, and the event is delivered once the piece waits for its turn.
// Only for a promise that one piece alone awaits and that no other task can
// see (not shared): nothing can tell that it settled before its event.
//
// Such a job, early, goes on ahead as long as it touches only its own data:
// it awaits without the turn what a function of the runtime gives it (a
// promise only it sees, async.c), and runs after itself the microtasks it
// queues, when they may run ahead too. Once it needs the turn, the main
// thread delivers its event: lends it the turn, its microtasks moving to the
// queue, which has nothing else then. A job that never needs it is never
// delivered: nothing of it could be seen.
//
// The events of the connections of servers come to the threads of the pool
// (net.c): the thread that takes one runs the piece that waits for it
// itself, as an early job the main thread hears of only once it needs the
// turn (tz_ahead_here); what it cannot run goes to the main thread, whose
// poll phase queues the reactions (tz_ahead_forward).

// The jobs that run ahead of their events, in the order the events came.
// Main thread only.
TzJob *tz_ahead_early_head, *tz_ahead_early_tail;

// What the threads of the pool hand to the main thread: early jobs that need
// the turn now, and promises whose reactions go to the queue (forward), in
// the order they came. Under the lock.
SRWLOCK tz_ahead_inbox_lock = SRWLOCK_INIT;
TzJob *tz_ahead_inbox_head, *tz_ahead_inbox_tail;

static void inbox_push(TzJob *job, int wake) {
  job->later = NULL;
  tz_work_ref_count(1); // the loop waits for it
  AcquireSRWLockExclusive(&tz_ahead_inbox_lock);
  if (tz_ahead_inbox_tail)
    tz_ahead_inbox_tail->later = job;
  else
    tz_ahead_inbox_head = job;
  tz_ahead_inbox_tail = job;
  ReleaseSRWLockExclusive(&tz_ahead_inbox_lock);
  if (wake)
    tz_io_wake_loop();
}

// Whether no other thread can reach the object: not shared, nor a member of
// a group that is.
static int is_private(void *object) {
  int64_t rc = ((TzObject *)object)->rc;
  if (tz_rc_is_member(rc))
    return !tz_group_published(object);
  return rc > 0;
}

int32_t tz_ahead_private(void *object) { return is_private(object); }

int32_t tz_ahead_early(void) {
  TzJob *job = tz_thread.job;
  return job && job->early;
}

int32_t tz_ahead_local(TzReactor *r) {
  TzJob *job = tz_thread.job;
  uintptr_t ahead = r->ahead;
  if (!job || !job->early || (ahead & ~EXAMINED) == 0 || ahead >= MAX_SITE)
    return 0;
  r->next = NULL;
  if (tz_thread.local_tail)
    tz_thread.local_tail->next = r;
  else
    tz_thread.local_head = r;
  tz_thread.local_tail = r;
  return 1;
}

// How often the pieces of each site that threads of the pool ran ahead of
// their events needed the turn after all: from 0 (never, of late) to
// TURNED_MAX. Those of a site whose pieces do go to the main thread, whose
// turn they would wait for: what a piece does with the turn the main thread
// cannot do meanwhile, and a handoff costs it more. One in TURNED_PROBE
// still runs ahead, to see whether that changed. Read and written by any
// thread, as an estimate; sites beyond the table run ahead.
#define TURNED_SITES 256
#define TURNED_MAX 32
#define TURNED_PROBE 64
int32_t tz_ahead_turned[TURNED_SITES];
TZ_THREAD_LOCAL uint32_t tz_ahead_probe;

// net.c, on a thread of the pool, for the promise that an event it took
// settled, its reactions held (and the promise with them): runs the piece
// that alone waits for it here, when that may run ahead (1); 0 otherwise.
// The promise is the runtime's own, which only that piece awaits, even when
// its group is shared.
int32_t tz_ahead_here(TzPromise *p) {
  TzReactor *r = p->head;
  if (!r || r != p->tail)
    return 0;
  uintptr_t ahead = r->ahead;
  if ((ahead & ~EXAMINED) == 0 || ahead >= MAX_SITE)
    return 0;
  uint32_t site = (uint32_t)(ahead & ~EXAMINED);
  int32_t *turned = site < TURNED_SITES ? &tz_ahead_turned[site] : NULL;
  if (turned && __atomic_load_n(turned, __ATOMIC_RELAXED) > TURNED_MAX / 2 &&
      ++tz_ahead_probe % TURNED_PROBE != 0 && !tz_pool_stress)
    return 0;
  TzJob *job = (TzJob *)malloc(sizeof(TzJob));
  if (!job)
    return 0;
  // As the promise's trigger would queue it (async.c), held by the queue
  // with what held it.
  p->head = p->tail = NULL;
  r->next = NULL;
  r->source = NULL;
  if (tz_rc_is_member(p->rc))
    tz_group_take(p, r->owner);
  job->reactor = r;
  job->source = p;
  job->state = JOB_RUNNING;
  job->site = (uint32_t)(ahead & ~EXAMINED);
  job->early = 1;
  job->listed = 0;
  job->forward = 0;
  job->later = NULL;
  r->ahead = (uintptr_t)job;
  __atomic_fetch_add(&tz_pool_stat_dispatched, 1, __ATOMIC_RELAXED);
  int waited = run_job(job);
  if (turned) {
    int32_t now = __atomic_load_n(turned, __ATOMIC_RELAXED);
    if (waited ? now < TURNED_MAX : now > 0)
      __atomic_store_n(turned, waited ? now + 1 : now - 1, __ATOMIC_RELAXED);
  }
  return 1;
}

// net.c: the reactions of `p` (held by the caller, which hands that over) go
// to the queue, in the poll phase of the main thread, as an event.
void tz_ahead_forward(TzPromise *p) {
  TzJob *job = (TzJob *)malloc(sizeof(TzJob));
  if (!job)
    tz_panic("out of memory", 13);
  memset(job, 0, sizeof *job);
  job->source = p;
  job->forward = 1;
  job->state = JOB_DONE;
  inbox_push(job, 1);
}

int32_t tz_ahead_defer(TzPromise *p) {
  TzReactor *r = p->head;
  if (!r || r != p->tail || !is_private(p) || (pool_room() <= 0 && !grow_pool()))
    return 0;
  uintptr_t site = r->ahead;
  if (site == 0 || site >= EXAMINED)
    return 0;
  Site *s = site_of((uint32_t)site);
  if (!s || !worth_a_thread(s))
    return 0;
  TzJob *job = (TzJob *)malloc(sizeof(TzJob));
  if (!job)
    return 0;
  // As the promise's trigger would queue it (async.c), held by the queue,
  // without the queue.
  p->head = p->tail = NULL;
  r->next = NULL;
  r->source = NULL;
  tz_obj_retain((TzObject *)p);
  if (tz_rc_is_member(p->rc))
    tz_group_take(p, r->owner);
  job->reactor = r;
  job->source = p;
  job->state = JOB_QUEUED;
  job->site = (uint32_t)site;
  job->early = 1;
  job->listed = 1;
  job->forward = 0;
  job->later = NULL;
  r->ahead = (uintptr_t)job;
  if (tz_ahead_early_tail)
    tz_ahead_early_tail->later = job;
  else
    tz_ahead_early_head = job;
  tz_ahead_early_tail = job;
  tz_work_ref_count(1); // the loop waits for its event
  ++tz_pool_stat_dispatched;
  push_job(job);
  return 1;
}

// The microtask of a job delivered: it lends the job the turn, and waits
// until its piece is done in its thread.
static void grant_turn(TzReactor *r, TzPromise *source) {
  (void)source;
  TzJob *job = (TzJob *)((char *)r - offsetof(TzJob, grant));
  set_state(&job->state, JOB_GRANTED);
  ++tz_pool_stat_waited;
  tz_pool_main_waits = job;
  for (;;) {
    int32_t now = __atomic_load_n(&job->state, __ATOMIC_ACQUIRE);
    if (now == JOB_DONE)
      break;
    wait_state(&job->state, now);
  }
  tz_pool_main_waits = NULL;
  learn(job->site, job->ahead);
  free(job);
}

// The events of the jobs that wait for their turn now, or are done: each a
// callback of the poll phase, its piece a microtask after it, which goes on
// in its thread with the turn lent to it; the microtasks it queued follow.
// Gives whether any is left that runs on.
int32_t tz_ahead_deliver(void) {
  if (__atomic_load_n(&tz_ahead_inbox_head, __ATOMIC_ACQUIRE)) {
    AcquireSRWLockExclusive(&tz_ahead_inbox_lock);
    TzJob *in = tz_ahead_inbox_head, *in_tail = tz_ahead_inbox_tail;
    tz_ahead_inbox_head = tz_ahead_inbox_tail = NULL;
    ReleaseSRWLockExclusive(&tz_ahead_inbox_lock);
    if (in) {
      if (tz_ahead_early_tail)
        tz_ahead_early_tail->later = in;
      else
        tz_ahead_early_head = in;
      tz_ahead_early_tail = in_tail;
    }
  }
  TzJob **at = &tz_ahead_early_head, *last = NULL;
  while (*at) {
    TzJob *job = *at;
    // One no thread took yet waits for one: the threads that hold the others
    // let go of them as their events come.
    int32_t state = __atomic_load_n(&job->state, __ATOMIC_ACQUIRE);
    if (state == JOB_RUNNING || state == JOB_QUEUED) {
      last = job;
      at = &job->later;
      continue;
    }
    *at = job->later;
    if (tz_ahead_early_tail == job)
      tz_ahead_early_tail = last;
    tz_work_ref_count(-1);
    if (job->forward) {
      TzPromise *p = job->source;
      free(job);
      tz_promise_trigger(p);
      tz_obj_release((TzObject *)p);
      tz_after_callback();
      continue;
    }
    if (state == JOB_WAITING) {
      // Its piece is the microtask after its event, as in node: what it
      // queues runs after it, the nextTick callbacks after those.
      memset(&job->grant, 0, sizeof job->grant);
      job->grant.run = grant_turn;
      tz_microtask(&job->grant);
    } else {
      learn(job->site, job->ahead);
      free(job);
    }
    tz_after_callback();
  }
  return tz_ahead_early_head != NULL;
}

static int32_t finish_job(TzReactor *r, TzPromise **source);

void tz_ahead_run(TzReactor *r, TzPromise *source) {
  uintptr_t ahead = r->ahead;
  uint32_t site = (uint32_t)(ahead & ~EXAMINED);
  if (ahead >= MAX_SITE) {
    // Read before the job may be gone: one taken back is freed by the
    // thread that pops it.
    site = ((TzJob *)ahead)->site;
    if (finish_job(r, &source)) {
      if (source)
        tz_obj_release((TzObject *)source);
      return;
    }
  }
  // On the main thread, timed for the site: every one while it is not known
  // to be short, then one in 64, which costs less than timing each.
  r->ahead = 0;
  Site *s = site_of(site);
  if (s && (!is_short(s) || (++s->counted & 63) == 0)) {
    int64_t start = ticks();
    r->run(r, source);
    learn(site, ticks() - start);
  } else {
    r->run(r, source);
  }
  if (source)
    tz_obj_release((TzObject *)source);
}

static int32_t finish_job(TzReactor *r, TzPromise **source) {
  TzJob *job = (TzJob *)r->ahead;
  *source = job->source;
  int32_t state = JOB_QUEUED;
  if (tz_pool_stress) {
    wait_state(&job->state, JOB_QUEUED);
    state = __atomic_load_n(&job->state, __ATOMIC_ACQUIRE);
  } else if (__atomic_compare_exchange_n(&job->state, &state, JOB_TAKEN, 0, __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE)) {
    // No thread started it: the main thread runs it, as it would have.
    // The job stays in the list of the pool: the thread that pops it frees
    // it.
    ++tz_pool_stat_taken;
    return 0;
  }
  r->ahead = 0;
  // Running, or waiting for its turn: lend it the turn.
  for (;;) {
    if (state == JOB_DONE)
      break;
    if (state == JOB_RUNNING || state == JOB_WAITING) {
      int32_t seen = state;
      if (__atomic_compare_exchange_n(&job->state, &state, JOB_GRANTED, 0, __ATOMIC_ACQ_REL,
                                      __ATOMIC_ACQUIRE)) {
        if (seen == JOB_WAITING)
          WakeByAddressAll(&job->state);
        break;
      }
      continue; // `state` holds what it is now
    }
    state = __atomic_load_n(&job->state, __ATOMIC_ACQUIRE);
  }
  // Until the piece is done, on its thread.
  ++tz_pool_stat_waited;
  tz_pool_main_waits = job;
  for (;;) {
    int32_t now = __atomic_load_n(&job->state, __ATOMIC_ACQUIRE);
    if (now == JOB_DONE)
      break;
    wait_state(&job->state, now);
  }
  tz_pool_main_waits = NULL;
  learn(job->site, job->ahead);
  free(job);
  return 1;
}

TZ_NOINLINE void tz_order_wait(void) {
  TzJob *job = tz_thread.job;
  if (!job) {
    // A thread without a job holds the turn while it runs: the main one. A
    // thread of the pool takes what comes for connections without one
    // (net.c), and touches nothing of the order then.
    if (tz_thread.worker)
      tz_panic("the parallel runtime: an ordered access outside a piece", 55);
    tz_thread.turn = 1;
    return;
  }
  // What waits for the turn holds no connection: the main thread may wait
  // for one (net.c).
  if (tz_thread.await_lock) {
    TzLock *lock = (TzLock *)tz_thread.await_lock;
    tz_thread.await_lock = NULL;
    --tz_thread.conn_locks;
    tz_unlock(lock);
  }
  if (tz_thread.conn_locks)
    tz_panic("the parallel runtime: waits for the turn holding a connection", 61);
  job->ahead = ticks() - job->started;
  // Ahead of an event the main thread did not hear of: now it does.
  if (job->early && !job->listed) {
    job->listed = 1;
    inbox_push(job, 0);
  }
  // With TZ_PARALLEL_STATS: where pieces stop running ahead, for the
  // developers of tz (the callers, as offsets in the executable).
  if (tz_pool_stat_where) {
    uintptr_t at = (uintptr_t)__builtin_return_address(0) - (uintptr_t)GetModuleHandleA(NULL);
    for (int i = 0; i < 8; ++i)
      if (tz_pool_stop_at[i].at == at || !tz_pool_stop_at[i].at) {
        tz_pool_stop_at[i].at = at;
        __atomic_fetch_add(&tz_pool_stop_at[i].count, 1, __ATOMIC_RELAXED);
        break;
      }
  }
  int32_t running = JOB_RUNNING;
  if (__atomic_compare_exchange_n(&job->state, &running, JOB_WAITING, 0, __ATOMIC_ACQ_REL,
                                  __ATOMIC_ACQUIRE)) {
    WakeByAddressAll(&job->state);
    if (job->early)
      tz_io_wake_loop(); // its event may come now
    if (tz_thread.index < 64)
      tz_thread_where[tz_thread.index] = "waits for its turn";
    wait_state(&job->state, JOB_WAITING);
    if (tz_thread.index < 64)
      tz_thread_where[tz_thread.index] = "has its turn";
  }
  // GRANTED: the main thread waits for this piece now.
  tz_thread.turn = 1;
  // The microtasks it queued ahead of its turn, which the queue, empty in
  // the poll phase, takes first.
  if (tz_thread.local_head) {
    if (tz_micro_tail)
      tz_micro_tail->next = tz_thread.local_head;
    else
      tz_micro_head = tz_thread.local_head;
    tz_micro_tail = tz_thread.local_tail;
    tz_thread.local_head = tz_thread.local_tail = NULL;
  }
}

void tz_order_enter(void) { TZ_ORDERED(); }

void tz_thread_yield(void) { SwitchToThread(); }

#else

void tz_thread_yield(void) {}

void tz_parallel_init(void) {}

void tz_thread_register(void) {}

int32_t tz_thread_total(void) { return 1; }

TzThread *tz_thread_at(int32_t i) {
  (void)i;
  return &tz_thread;
}

int64_t tz_thread_sum(size_t offset) { return *(const int64_t *)((const char *)&tz_thread + offset); }

void tz_order_enter(void) {}

#endif
