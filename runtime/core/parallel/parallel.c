#include "runtime/core/parallel/parallel.h"
#include "runtime/core/async/async.h"
#include "runtime/core/closure/closure.h"
#include "runtime/core/group/group.h"
#include "runtime/core/io/io.h"
#include "runtime/core/output/output.h"
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

TzThread *tz_thread_list[MAX_THREADS];
int32_t tz_thread_count;

void tz_thread_register(void) {
  int32_t i = __atomic_fetch_add(&tz_thread_count, 1, __ATOMIC_ACQ_REL);
  if (i >= MAX_THREADS) {
    tz_panic("too many threads", 16);
  }
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
    if (t) {
      sum += __atomic_load_n((const int64_t *)((const char *)t + offset), __ATOMIC_RELAXED);
    }
  }

  return sum;
}

enum { JOB_QUEUED = 1, JOB_RUNNING, JOB_WAITING, JOB_GRANTED, JOB_DONE, JOB_TAKEN };

typedef struct TzJob {
  TzReactor *reactor;
  TzPromise *source;
  struct TzJob *next;
  int32_t state;
  int32_t listed;
  int32_t forward;
  TzReactor grant;
  uint32_t site;
  int64_t started;
  int64_t ahead;
  int32_t early;
  struct TzJob *later;
} TzJob;

static inline int64_t ticks(void) {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);

  return now.QuadPart;
}

static void wait_state(int32_t *state, int32_t seen) {
  for (int spin = 0; spin < 4000; ++spin) {
    if (__atomic_load_n(state, __ATOMIC_ACQUIRE) != seen) {
      return;
    }
    YieldProcessor();
  }
  while (__atomic_load_n(state, __ATOMIC_ACQUIRE) == seen) {
    WaitOnAddress(state, &seen, sizeof seen, INFINITE);
  }
}

static void set_state(int32_t *state, int32_t value) {
  __atomic_store_n(state, value, __ATOMIC_RELEASE);
  WakeByAddressAll(state);
}

int32_t tz_ahead_on;

HANDLE tz_pool_port;
#define POOL_JOB ((ULONG_PTR)1)
SRWLOCK tz_pool_lock = SRWLOCK_INIT;
int32_t tz_pool_queued;
int32_t tz_pool_idle;
int32_t tz_pool_started;
int32_t tz_pool_running;
int32_t tz_pool_size = -1;
int32_t tz_pool_base;
int64_t tz_pool_spare_at;

static int32_t pool_room(void) {
  return tz_pool_size - __atomic_load_n(&tz_pool_running, __ATOMIC_RELAXED) -
         __atomic_load_n(&tz_pool_queued, __ATOMIC_RELAXED);
}

void tz_heap_collect(void);

typedef struct {
  uint32_t runs;
  uint32_t probe;
  uint32_t counted;
  uint32_t every;
  double average;
} Site;
extern Site *tz_ahead_sites;
extern uint32_t tz_ahead_site_count;

int64_t tz_pool_stat_dispatched, tz_pool_stat_ran, tz_pool_stat_taken, tz_pool_stat_waited;

int32_t tz_pool_stress;

TzJob *tz_pool_main_waits;
extern int32_t tz_group_lock_owner;
extern int32_t tz_str_flatten_lock;
const char *tz_thread_where[64];

static DWORD WINAPI watchdog(void *arg) {
  Sleep((DWORD)(intptr_t)arg * 1000);
  TzJob *j = tz_pool_main_waits;
  fprintf(stderr,
          "tz watchdog: main waits for job %p (state %d), group lock %d, flatten lock %d, "
          "queued %d idle %d running %d started %d\n",
          (void *)j, j ? j->state : -1, tz_group_lock_owner, tz_str_flatten_lock, tz_pool_queued,
          tz_pool_idle, tz_pool_running, tz_pool_started);
  for (int i = 0; i < 64; ++i) {
    if (tz_thread_where[i]) {
      fprintf(stderr, "  thread %d: %s\n", i, tz_thread_where[i]);
    }
  }
  for (int32_t i = 1; i < tz_thread_total(); ++i) {
    TzThread *t = tz_thread_at(i);
    TzJob *job = t ? t->job : NULL;
    if (job) {
      fprintf(stderr, "  thread %d runs job %p: state %d, early %d, reactor %p\n", i, (void *)job,
              job->state, job->early, (void *)job->reactor);
    }
  }
  extern TzJob *tz_ahead_early_head;
  for (TzJob *job = tz_ahead_early_head; job; job = job->later) {
    fprintf(stderr, "  early job %p: state %d\n", (void *)job, job->state);
  }
  extern TzReactor *tz_micro_head;
  fprintf(stderr, "  microtask queue %s; pending I/O %d\n",
          tz_micro_head ? "has reactors" : "empty", tz_io_alive());
  fflush(stderr);

  return 0;
}

int32_t tz_pool_stat_where;
struct {
  uintptr_t at;
  int64_t count;
} tz_pool_stop_at[8];

static void print_stats(void) {
  for (int i = 0; i < 8 && tz_pool_stop_at[i].at; ++i) {
    fprintf(stderr, "  stopped ahead at +0x%llx: %lld times\n",
            (unsigned long long)tz_pool_stop_at[i].at, (long long)tz_pool_stop_at[i].count);
  }
  fprintf(stderr, "tz: %lld pieces handed out, %lld ran ahead, %lld taken back, %lld waited for\n",
          (long long)tz_pool_stat_dispatched, (long long)tz_pool_stat_ran,
          (long long)tz_pool_stat_taken, (long long)tz_pool_stat_waited);
  for (uint32_t i = 0; i < tz_ahead_site_count && i < 8; ++i) {
    if (tz_ahead_sites[i].runs) {
      fprintf(stderr, "  site %u: %u runs, %.0f ticks on average\n", i + 1, tz_ahead_sites[i].runs,
              tz_ahead_sites[i].average);
    }
  }
}

extern TzReactor *tz_micro_head, *tz_micro_tail;

static void run_early(TzJob *job) {
  TzReactor *r = job->reactor;
  TzPromise *source = job->source;
  TzObject *owner = r->owner;
  r->run(r, source);
  if (source) {
    tz_obj_release((TzObject *)source);
  }
  tz_obj_release(owner);
  while (tz_thread.local_head) {
    TzReactor *next = tz_thread.local_head;
    tz_thread.local_head = next->next;
    if (!tz_thread.local_head) {
      tz_thread.local_tail = NULL;
    }
    TzPromise *got = next->source;
    TzObject *of = next->owner;
    next->source = NULL;
    next->run(next, got);
    if (got) {
      tz_obj_release((TzObject *)got);
    }
    tz_obj_release(of);
  }
}

static int run_job(TzJob *job) {
  TzReactor *r = job->reactor;
  job->started = ticks();
  __atomic_store_n(&tz_thread.job, job, __ATOMIC_RELEASE);
  tz_thread.turn = 0;
  job->ahead = -1;
  __atomic_fetch_add(&tz_pool_stat_ran, 1, __ATOMIC_RELAXED);
  if (job->early) {
    run_early(job);
  } else {
    r->run(r, job->source);
  }
  tz_io_send_pending();
  if (job->ahead < 0) {
    job->ahead = ticks() - job->started;
  }
  tz_thread.turn = 0;
  __atomic_store_n(&tz_thread.job, NULL, __ATOMIC_RELEASE);
  int32_t early = job->early;
  if (early && !job->listed) {
    free(job);
    return 0;
  }
  set_state(&job->state, JOB_DONE);
  if (early) {
    tz_io_wake_loop();
  }

  return 1;
}

static DWORD WINAPI worker_main(void *arg) {
  (void)arg;
  tz_thread_register();
  tz_thread_stack_init();
  tz_thread.turn = 0;
  tz_thread.worker = 1;
  for (;;) {
    if (__atomic_load_n(&tz_thread.heap.remote, __ATOMIC_RELAXED)) {
      tz_heap_collect();
    }
    OVERLAPPED_ENTRY entry;
    ULONG count = 0;
    __atomic_fetch_add(&tz_pool_idle, 1, __ATOMIC_RELAXED);
    BOOL ok = GetQueuedCompletionStatusEx(tz_pool_port, &entry, 1, &count, INFINITE, FALSE);
    __atomic_fetch_sub(&tz_pool_idle, 1, __ATOMIC_RELAXED);
    if (!ok || count == 0) {
      continue;
    }
    __atomic_fetch_add(&tz_pool_running, 1, __ATOMIC_RELAXED);
    if (entry.lpCompletionKey == POOL_JOB) {
      TzJob *job = (TzJob *)entry.lpOverlapped;
      __atomic_fetch_sub(&tz_pool_queued, 1, __ATOMIC_RELAXED);
      int32_t queued = JOB_QUEUED;
      if (__atomic_compare_exchange_n(&job->state, &queued, JOB_RUNNING, 0, __ATOMIC_ACQ_REL,
                                      __ATOMIC_ACQUIRE)) {
        WakeByAddressAll(&job->state);
        run_job(job);
      } else {
        free(job);
      }
    } else {
      ((void (*)(OVERLAPPED_ENTRY *))entry.lpCompletionKey)(&entry);
      tz_io_send_pending();
    }
    __atomic_fetch_sub(&tz_pool_running, 1, __ATOMIC_RELAXED);
  }

  return 0;
}

static int32_t pool_size(void) {
  if (tz_pool_size >= 0) {
    return tz_pool_size;
  }
  int32_t threads = 0;
  const char *value = getenv("TZ_THREADS");
  if (value && *value) {
    threads = atoi(value);
  }
  if (threads <= 0) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    threads = info.dwNumberOfProcessors > 0 ? (int32_t)info.dwNumberOfProcessors : 1;
  }
  if (threads > MAX_THREADS / 2) {
    threads = MAX_THREADS / 2;
  }
  tz_pool_size = threads - 1;
  tz_pool_base = tz_pool_size;
  if (tz_pool_size > 0) {
    tz_pool_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, MAX_THREADS);
    if (!tz_pool_port) {
      tz_pool_size = tz_pool_base = 0;
    }
  }
  if ((tz_pool_stat_where = getenv("TZ_PARALLEL_STATS") != NULL)) {
    atexit(print_stats);
  }
  tz_pool_stress = getenv("TZ_PARALLEL_STRESS") != NULL;
  if (getenv("TZ_PARALLEL_WATCHDOG")) {
    HANDLE t = CreateThread(NULL, 0, watchdog,
                            (void *)(intptr_t)atoi(getenv("TZ_PARALLEL_WATCHDOG")), 0, NULL);
    if (t) {
      CloseHandle(t);
    }
  }

  return tz_pool_size;
}

static void start_one(int32_t limit) {
  AcquireSRWLockExclusive(&tz_pool_lock);
  if (tz_pool_started < limit) {
    HANDLE thread = CreateThread(NULL, (SIZE_T)1 << 30, worker_main, NULL,
                                 STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (thread) {
      CloseHandle(thread);
      __atomic_store_n(&tz_pool_started, tz_pool_started + 1, __ATOMIC_RELAXED);
    }
  }
  ReleaseSRWLockExclusive(&tz_pool_lock);
}

static void start_thread(void) {
  if (__atomic_load_n(&tz_pool_started, __ATOMIC_RELAXED) >= tz_pool_size ||
      __atomic_load_n(&tz_pool_idle, __ATOMIC_RELAXED) >=
          __atomic_load_n(&tz_pool_queued, __ATOMIC_RELAXED)) {
    return;
  }
  start_one(tz_pool_size);
}

static void push_job(TzJob *job) {
  job->next = NULL;
  __atomic_fetch_add(&tz_pool_queued, 1, __ATOMIC_RELAXED);
  start_thread();
  PostQueuedCompletionStatus(tz_pool_port, 0, POOL_JOB, (OVERLAPPED *)job);
}

void *tz_pool_port_handle(void) {
  if (!tz_ahead_on) {
    return NULL;
  }
  while (__atomic_load_n(&tz_pool_started, __ATOMIC_RELAXED) < tz_pool_base) {
    start_one(tz_pool_base);
  }

  return tz_pool_port;
}

void tz_parallel_init(void) { tz_ahead_on = pool_size() > 0; }

Site *tz_ahead_sites;
uint32_t tz_ahead_site_count;
double tz_ahead_worth;

static Site *site_of(uint32_t n) {
  if (n > tz_ahead_site_count) {
    uint32_t count = tz_ahead_site_count ? tz_ahead_site_count : 64;
    while (count < n) {
      count *= 2;
    }
    Site *grown = (Site *)realloc(tz_ahead_sites, count * sizeof(Site));
    if (!grown) {
      return NULL;
    }
    memset(grown + tz_ahead_site_count, 0, (count - tz_ahead_site_count) * sizeof(Site));
    tz_ahead_sites = grown;
    tz_ahead_site_count = count;
  }

  return &tz_ahead_sites[n - 1];
}

static void learn(uint32_t n, int64_t spent) {
  Site *s = site_of(n);
  if (!s) {
    return;
  }
  s->average = s->runs ? s->average * 0.75 + (double)spent * 0.25 : (double)spent;
  ++s->runs;
  if (s->average >= tz_ahead_worth) {
    s->every = 0;
  }
}

static int is_short(const Site *s) { return s->runs >= 4 && s->average < tz_ahead_worth; }

static int worth_a_thread(Site *s) {
  if (tz_pool_stress || !is_short(s)) {
    return 1;
  }
  if (s->every < 256) {
    s->every = 256;
  }
  if (++s->probe < s->every) {
    return 0;
  }
  s->probe = 0;
  if (s->every < 65536) {
    s->every *= 2;
  }

  return 1;
}

#define AHEAD_WINDOW 64
#define MAX_SITE ((uintptr_t)1 << 20)
#define EXAMINED ((uintptr_t)1 << 19)

#define SPARE_AFTER 10e-3

static int grow_pool(void) {
  if (tz_pool_base <= 0 || tz_pool_size >= 2 * tz_pool_base) {
    return 0;
  }
  int64_t now = ticks();
  LARGE_INTEGER frequency;
  QueryPerformanceFrequency(&frequency);
  int64_t after = (int64_t)((double)frequency.QuadPart * SPARE_AFTER);
  if (now - tz_pool_spare_at < after) {
    return 0;
  }
  tz_pool_spare_at = now;
  int32_t n = tz_thread_total();
  for (int32_t i = 1; i < n; ++i) {
    TzThread *t = tz_thread_at(i);
    TzJob *job = t ? __atomic_load_n(&t->job, __ATOMIC_ACQUIRE) : NULL;
    if (!job) {
      return 0;
    }
    int64_t started = __atomic_load_n(&job->started, __ATOMIC_RELAXED);
    if (__atomic_load_n(&job->state, __ATOMIC_ACQUIRE) != JOB_RUNNING || now - started < after) {
      return 0;
    }
  }
  __atomic_fetch_add(&tz_pool_size, 1, __ATOMIC_RELAXED);

  return 1;
}

TzReactor *tz_ahead_last;

void tz_ahead_popped(TzReactor *r) {
  if (r == tz_ahead_last) {
    tz_ahead_last = NULL;
  }
}

void tz_ahead_dispatch(TzReactor *head) {
  if (pool_room() <= 0 && !grow_pool()) {
    return;
  }
  if (tz_ahead_worth == 0) {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    tz_ahead_worth = (double)frequency.QuadPart * 30e-6;
  }
  int scanned = 0;
  TzReactor *first = tz_ahead_last ? tz_ahead_last->next : head->next;
  for (TzReactor *r = first; r && scanned < AHEAD_WINDOW; r = r->next, ++scanned) {
    uintptr_t site = r->ahead;
    if (site != 0 && site < EXAMINED) {
      Site *s = site_of((uint32_t)site);
      if (!s) {
        return;
      }
      if (!worth_a_thread(s)) {
        r->ahead = site | EXAMINED;
      } else {
        if (pool_room() <= 0 && !grow_pool()) {
          return;
        }
        TzJob *job = (TzJob *)malloc(sizeof(TzJob));
        if (!job) {
          return;
        }
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

int32_t tz_ahead_keep(int32_t site) {
  Site *s = site_of((uint32_t)site);
  if (!s || tz_pool_stress || !is_short(s)) {
    return site;
  }

  return (++s->counted & 63) == 0 ? site : (int32_t)(site | EXAMINED);
}

TzJob *tz_ahead_early_head, *tz_ahead_early_tail;

SRWLOCK tz_ahead_inbox_lock = SRWLOCK_INIT;
TzJob *tz_ahead_inbox_head, *tz_ahead_inbox_tail;

static void inbox_push(TzJob *job, int wake) {
  job->later = NULL;
  tz_work_ref_count(1);
  AcquireSRWLockExclusive(&tz_ahead_inbox_lock);
  if (tz_ahead_inbox_tail) {
    tz_ahead_inbox_tail->later = job;
  } else {
    tz_ahead_inbox_head = job;
  }
  tz_ahead_inbox_tail = job;
  ReleaseSRWLockExclusive(&tz_ahead_inbox_lock);
  if (wake) {
    tz_io_wake_loop();
  }
}

static int is_private(void *object) {
  int64_t rc = ((TzObject *)object)->rc;
  if (tz_rc_is_member(rc)) {
    return !tz_group_published(object);
  }

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
  if (!job || !job->early || (ahead & ~EXAMINED) == 0 || ahead >= MAX_SITE) {
    return 0;
  }
  r->next = NULL;
  if (tz_thread.local_tail) {
    tz_thread.local_tail->next = r;
  } else {
    tz_thread.local_head = r;
  }
  tz_thread.local_tail = r;

  return 1;
}

#define TURNED_SITES 256
#define TURNED_MAX 32
#define TURNED_PROBE 64
int32_t tz_ahead_turned[TURNED_SITES];
TZ_THREAD_LOCAL uint32_t tz_ahead_probe;

int32_t tz_ahead_here(TzPromise *p) {
  TzReactor *r = p->head;
  if (!r || r != p->tail) {
    return 0;
  }
  uintptr_t ahead = r->ahead;
  if ((ahead & ~EXAMINED) == 0 || ahead >= MAX_SITE) {
    return 0;
  }
  uint32_t site = (uint32_t)(ahead & ~EXAMINED);
  int32_t *turned = site < TURNED_SITES ? &tz_ahead_turned[site] : NULL;
  if (turned && __atomic_load_n(turned, __ATOMIC_RELAXED) > TURNED_MAX / 2 &&
      ++tz_ahead_probe % TURNED_PROBE != 0 && !tz_pool_stress) {
    return 0;
  }
  TzJob *job = (TzJob *)malloc(sizeof(TzJob));
  if (!job) {
    return 0;
  }
  p->head = p->tail = NULL;
  r->next = NULL;
  if (r->source == TZ_REACTION_WITHIN) {
    tz_group_take(p, r->owner);
  }
  r->source = NULL;
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
    if (waited ? now < TURNED_MAX : now > 0) {
      __atomic_store_n(turned, waited ? now + 1 : now - 1, __ATOMIC_RELAXED);
    }
  }

  return 1;
}

void tz_ahead_forward(TzPromise *p) {
  TzJob *job = (TzJob *)malloc(sizeof(TzJob));
  if (!job) {
    tz_panic("out of memory", 13);
  }
  memset(job, 0, sizeof *job);
  job->source = p;
  job->forward = 1;
  job->state = JOB_DONE;
  inbox_push(job, 1);
}

int32_t tz_ahead_defer(TzPromise *p) {
  TzReactor *r = p->head;
  if (!r || r != p->tail || !is_private(p) || (pool_room() <= 0 && !grow_pool())) {
    return 0;
  }
  uintptr_t site = r->ahead;
  if (site == 0 || site >= EXAMINED) {
    return 0;
  }
  Site *s = site_of((uint32_t)site);
  if (!s || !worth_a_thread(s)) {
    return 0;
  }
  TzJob *job = (TzJob *)malloc(sizeof(TzJob));
  if (!job) {
    return 0;
  }
  p->head = p->tail = NULL;
  r->next = NULL;
  if (r->source == TZ_REACTION_WITHIN) {
    tz_group_take(p, r->owner);
  }
  r->source = NULL;
  tz_obj_retain((TzObject *)p);
  job->reactor = r;
  job->source = p;
  job->state = JOB_QUEUED;
  job->site = (uint32_t)site;
  job->early = 1;
  job->listed = 1;
  job->forward = 0;
  job->later = NULL;
  r->ahead = (uintptr_t)job;
  if (tz_ahead_early_tail) {
    tz_ahead_early_tail->later = job;
  } else {
    tz_ahead_early_head = job;
  }
  tz_ahead_early_tail = job;
  tz_work_ref_count(1);
  ++tz_pool_stat_dispatched;
  push_job(job);

  return 1;
}

static void grant_turn(TzReactor *r, TzPromise *source) {
  (void)source;
  TzJob *job = (TzJob *)((char *)r - offsetof(TzJob, grant));
  tz_io_send_pending();
  set_state(&job->state, JOB_GRANTED);
  ++tz_pool_stat_waited;
  tz_pool_main_waits = job;
  for (;;) {
    int32_t now = __atomic_load_n(&job->state, __ATOMIC_ACQUIRE);
    if (now == JOB_DONE) {
      break;
    }
    wait_state(&job->state, now);
  }
  tz_pool_main_waits = NULL;
  learn(job->site, job->ahead);
  free(job);
}

int32_t tz_ahead_deliver(void) {
  if (__atomic_load_n(&tz_ahead_inbox_head, __ATOMIC_ACQUIRE)) {
    AcquireSRWLockExclusive(&tz_ahead_inbox_lock);
    TzJob *in = tz_ahead_inbox_head, *in_tail = tz_ahead_inbox_tail;
    tz_ahead_inbox_head = tz_ahead_inbox_tail = NULL;
    ReleaseSRWLockExclusive(&tz_ahead_inbox_lock);
    if (in) {
      if (tz_ahead_early_tail) {
        tz_ahead_early_tail->later = in;
      } else {
        tz_ahead_early_head = in;
      }
      tz_ahead_early_tail = in_tail;
    }
  }
  TzJob **at = &tz_ahead_early_head, *last = NULL;
  while (*at) {
    TzJob *job = *at;
    int32_t state = __atomic_load_n(&job->state, __ATOMIC_ACQUIRE);
    if (state == JOB_RUNNING || state == JOB_QUEUED) {
      last = job;
      at = &job->later;
      continue;
    }
    *at = job->later;
    if (tz_ahead_early_tail == job) {
      tz_ahead_early_tail = last;
    }
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
    site = ((TzJob *)ahead)->site;
    if (finish_job(r, &source)) {
      if (source) {
        tz_obj_release((TzObject *)source);
      }
      return;
    }
  }
  r->ahead = 0;
  Site *s = site_of(site);
  if (s && (!is_short(s) || (++s->counted & 63) == 0)) {
    int64_t start = ticks();
    r->run(r, source);
    learn(site, ticks() - start);
  } else {
    r->run(r, source);
  }
  if (source) {
    tz_obj_release((TzObject *)source);
  }
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
    ++tz_pool_stat_taken;
    return 0;
  }
  r->ahead = 0;
  for (;;) {
    if (state == JOB_DONE) {
      break;
    }
    if (state == JOB_RUNNING || state == JOB_WAITING) {
      int32_t seen = state;
      if (__atomic_compare_exchange_n(&job->state, &state, JOB_GRANTED, 0, __ATOMIC_ACQ_REL,
                                      __ATOMIC_ACQUIRE)) {
        if (seen == JOB_WAITING) {
          WakeByAddressAll(&job->state);
        }
        break;
      }
      continue;
    }
    state = __atomic_load_n(&job->state, __ATOMIC_ACQUIRE);
  }
  ++tz_pool_stat_waited;
  tz_pool_main_waits = job;
  tz_io_send_pending();
  for (;;) {
    int32_t now = __atomic_load_n(&job->state, __ATOMIC_ACQUIRE);
    if (now == JOB_DONE) {
      break;
    }
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
    if (tz_thread.worker) {
      tz_panic("the parallel runtime: an ordered access outside a piece", 55);
    }
    tz_thread.turn = 1;
    return;
  }
  if (tz_thread.await_lock) {
    TzLock *lock = (TzLock *)tz_thread.await_lock;
    tz_thread.await_lock = NULL;
    --tz_thread.conn_locks;
    tz_unlock(lock);
  }
  if (tz_thread.conn_locks) {
    tz_panic("the parallel runtime: waits for the turn holding a connection", 61);
  }
  tz_io_send_pending();
  if (tz_thread.turn) {
    return;
  }
  job->ahead = ticks() - job->started;
  if (job->early && !job->listed) {
    job->listed = 1;
    inbox_push(job, 0);
  }
  if (tz_pool_stat_where) {
    uintptr_t at = (uintptr_t)__builtin_return_address(0) - (uintptr_t)GetModuleHandleA(NULL);
    for (int i = 0; i < 8; ++i) {
      if (tz_pool_stop_at[i].at == at || !tz_pool_stop_at[i].at) {
        tz_pool_stop_at[i].at = at;
        __atomic_fetch_add(&tz_pool_stop_at[i].count, 1, __ATOMIC_RELAXED);
        break;
      }
    }
  }
  int32_t running = JOB_RUNNING;
  if (__atomic_compare_exchange_n(&job->state, &running, JOB_WAITING, 0, __ATOMIC_ACQ_REL,
                                  __ATOMIC_ACQUIRE)) {
    WakeByAddressAll(&job->state);
    if (job->early) {
      tz_io_wake_loop();
    }
    if (tz_thread.index < 64) {
      tz_thread_where[tz_thread.index] = "waits for its turn";
    }
    wait_state(&job->state, JOB_WAITING);
    if (tz_thread.index < 64) {
      tz_thread_where[tz_thread.index] = "has its turn";
    }
  }
  tz_thread.turn = 1;
  if (tz_thread.local_head) {
    if (tz_micro_tail) {
      tz_micro_tail->next = tz_thread.local_head;
    } else {
      tz_micro_head = tz_thread.local_head;
    }
    tz_micro_tail = tz_thread.local_tail;
    tz_thread.local_head = tz_thread.local_tail = NULL;
  }
}

void tz_order_enter(void) { TZ_ORDERED(); }

TZ_HOT void tz_call_awaited(void) { tz_thread.awaited_call = 1; }

TZ_HOT int32_t tz_take_awaited(void) {
  int32_t awaited = tz_thread.awaited_call;
  tz_thread.awaited_call = 0;

  return awaited;
}

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

int64_t tz_thread_sum(size_t offset) {
  return *(const int64_t *)((const char *)&tz_thread + offset);
}

void tz_order_enter(void) {}

void tz_call_awaited(void) {}

int32_t tz_take_awaited(void) { return 0; }

#endif
