#include "runtime/core/io/io.h"
#include "runtime/core/async/async.h"
#include "runtime/core/closure/closure.h"
#include "runtime/core/group/group.h"
#include "runtime/core/output/output.h"
#include "runtime/core/tzrt/tzrt.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#endif

#define MAX_THREADS 1024

#if TZ_PARALLEL
#define PENDING_ADD(n) __atomic_add_fetch(&tz_io_pending, (n), __ATOMIC_RELAXED)
#define PENDING() __atomic_load_n(&tz_io_pending, __ATOMIC_RELAXED)
#else
#define PENDING_ADD(n) (tz_io_pending += (n))
#define PENDING() tz_io_pending
#endif

TzWork *tz_io_queue_head, *tz_io_queue_tail;
TzWork *tz_io_due_head, *tz_io_due_tail;
int64_t tz_io_pending;
uint64_t tz_io_polls;
int32_t tz_io_threads;

#ifdef _WIN32
HANDLE tz_io_port;
SRWLOCK tz_io_lock = SRWLOCK_INIT;
SRWLOCK tz_io_remote_lock = SRWLOCK_INIT;
TzWork *tz_io_remote_head, *tz_io_remote_tail;
CONDITION_VARIABLE tz_io_wake = CONDITION_VARIABLE_INIT;
HANDLE tz_io_timer;
#else
pthread_mutex_t tz_io_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t tz_io_wake = PTHREAD_COND_INITIALIZER;
TzWork *tz_io_back_head, *tz_io_back_tail;
int tz_io_pipe[2] = {-1, -1};
#endif

static int32_t pool_size(void) {
  const char *value = getenv("UV_THREADPOOL_SIZE");
  int32_t n = value ? (int32_t)atoi(value) : 4;
  if (n <= 0) {
    n = 1;
  }

  return n > MAX_THREADS ? MAX_THREADS : n;
}

static TzWork *take_work(void) {
  TzWork *w = tz_io_queue_head;
  tz_io_queue_head = w->next;
  if (!tz_io_queue_head) {
    tz_io_queue_tail = NULL;
  }
  w->next = NULL;

  return w;
}

#ifdef _WIN32

static DWORD WINAPI pool_thread(void *arg) {
  (void)arg;
  for (;;) {
    AcquireSRWLockExclusive(&tz_io_lock);
    while (!tz_io_queue_head) {
      SleepConditionVariableSRW(&tz_io_wake, &tz_io_lock, INFINITE, 0);
    }
    TzWork *w = take_work();
    ReleaseSRWLockExclusive(&tz_io_lock);
    w->work(w);
    PostQueuedCompletionStatus(tz_io_port, 0, 0, (OVERLAPPED *)w);
  }
}

static void start_port(void) {
  tz_io_port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
  if (!tz_io_port) {
    tz_panicf("cannot create the completion port of the loop (error %lu)", GetLastError());
  }
}

static void start_pool(void) {
  int32_t n = pool_size();
  for (int32_t i = 0; i < n; ++i) {
    HANDLE thread = CreateThread(NULL, 0, pool_thread, NULL, 0, NULL);
    if (!thread) {
      tz_panicf("cannot start a thread of the pool (error %lu)", GetLastError());
    }
    CloseHandle(thread);
  }
  tz_io_threads = n;
}

static void push_work(TzWork *w) {
  AcquireSRWLockExclusive(&tz_io_lock);
  if (tz_io_queue_tail) {
    tz_io_queue_tail->next = w;
  } else {
    tz_io_queue_head = w;
  }
  tz_io_queue_tail = w;
  ReleaseSRWLockExclusive(&tz_io_lock);
  WakeConditionVariable(&tz_io_wake);
}

static void CALLBACK timer_fired(void *arg, DWORD low, DWORD high) {
  (void)arg;
  (void)low;
  (void)high;
}

static void sleep_ms(int64_t ms) {
  if (!tz_io_timer) {
    tz_io_timer =
        CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  }
  if (tz_io_timer) {
    LARGE_INTEGER due;
    due.QuadPart = -ms * 10000;
    if (SetWaitableTimer(tz_io_timer, &due, 0, NULL, NULL, FALSE)) {
      WaitForSingleObject(tz_io_timer, INFINITE);
      return;
    }
  }
  Sleep((DWORD)ms);
}

static void gather(int64_t timeout) {
  if (!tz_io_port) {
    if (timeout > 0) {
      sleep_ms(timeout);
    }
    return;
  }
  OVERLAPPED_ENTRY entries[64];
  DWORD wait = timeout < 0 ? INFINITE : 0;
  BOOL alertable = FALSE, timed = FALSE;
  if (timeout > 0) {
    if (!tz_io_timer) {
      tz_io_timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                           TIMER_ALL_ACCESS);
    }
    LARGE_INTEGER due;
    due.QuadPart = -timeout * 10000;
    timed = tz_io_timer && SetWaitableTimer(tz_io_timer, &due, 0, timer_fired, NULL, FALSE);
    if (timed) {
      wait = INFINITE;
      alertable = TRUE;
    } else {
      wait = (DWORD)timeout;
    }
  }
  for (;;) {
    ULONG count = 0;
    BOOL ok = GetQueuedCompletionStatusEx(tz_io_port, entries, 64, &count, wait, alertable);
    if (!ok) {
      count = 0;
    }
    for (ULONG i = 0; i < count; ++i) {
      if (entries[i].lpCompletionKey) {
        ((void (*)(OVERLAPPED_ENTRY *))entries[i].lpCompletionKey)(&entries[i]);
        continue;
      }
      TzWork *w = (TzWork *)entries[i].lpOverlapped;
      w->next = NULL;
      w->due = w->queued + (w->turns ? w->turns : 1);
      if (tz_io_due_tail) {
        tz_io_due_tail->next = w;
      } else {
        tz_io_due_head = w;
      }
      tz_io_due_tail = w;
    }
    if (count < 64) {
      break;
    }
    wait = 0;
    alertable = FALSE;
  }
  if (__atomic_load_n(&tz_io_remote_head, __ATOMIC_ACQUIRE)) {
    AcquireSRWLockExclusive(&tz_io_remote_lock);
    TzWork *back = tz_io_remote_head;
    tz_io_remote_head = tz_io_remote_tail = NULL;
    ReleaseSRWLockExclusive(&tz_io_remote_lock);
    while (back) {
      TzWork *w = back;
      back = w->next;
      w->next = NULL;
      w->due = w->queued + (w->turns ? w->turns : 1);
      if (tz_io_due_tail) {
        tz_io_due_tail->next = w;
      } else {
        tz_io_due_head = w;
      }
      tz_io_due_tail = w;
    }
  }
  if (timed) {
    CancelWaitableTimer(tz_io_timer);
    SleepEx(0, TRUE);
  }
}

#else

static void *pool_thread(void *arg) {
  (void)arg;
  for (;;) {
    pthread_mutex_lock(&tz_io_lock);
    while (!tz_io_queue_head) {
      pthread_cond_wait(&tz_io_wake, &tz_io_lock);
    }
    TzWork *w = take_work();
    pthread_mutex_unlock(&tz_io_lock);
    w->work(w);
    pthread_mutex_lock(&tz_io_lock);
    int wake = tz_io_back_head == NULL;
    if (tz_io_back_tail) {
      tz_io_back_tail->next = w;
    } else {
      tz_io_back_head = w;
    }
    tz_io_back_tail = w;
    pthread_mutex_unlock(&tz_io_lock);
    if (wake) {
      char byte = 1;
      while (write(tz_io_pipe[1], &byte, 1) < 0 && errno == EINTR) {
      }
    }
  }

  return NULL;
}

static void start_port(void) {
  if (pipe(tz_io_pipe) != 0) {
    tz_panicf("cannot create the pipe of the loop (errno %d)", errno);
  }
  fcntl(tz_io_pipe[0], F_SETFL, fcntl(tz_io_pipe[0], F_GETFL) | O_NONBLOCK);
  fcntl(tz_io_pipe[0], F_SETFD, FD_CLOEXEC);
  fcntl(tz_io_pipe[1], F_SETFD, FD_CLOEXEC);
}

static void start_pool(void) {
  int32_t n = pool_size();
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  for (int32_t i = 0; i < n; ++i) {
    pthread_t thread;
    if (pthread_create(&thread, &attr, pool_thread, NULL) != 0) {
      tz_panicf("cannot start a thread of the pool");
    }
  }
  pthread_attr_destroy(&attr);
  tz_io_threads = n;
}

static void push_work(TzWork *w) {
  pthread_mutex_lock(&tz_io_lock);
  if (tz_io_queue_tail) {
    tz_io_queue_tail->next = w;
  } else {
    tz_io_queue_head = w;
  }
  tz_io_queue_tail = w;
  pthread_mutex_unlock(&tz_io_lock);
  pthread_cond_signal(&tz_io_wake);
}

static void sleep_ms(int64_t ms) {
  struct timespec span = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000};
  while (nanosleep(&span, &span) != 0) {
  }
}

static void gather(int64_t timeout) {
  if (tz_io_pipe[0] < 0) {
    if (timeout > 0) {
      sleep_ms(timeout);
    }
    return;
  }
  struct pollfd p = {tz_io_pipe[0], POLLIN, 0};
  while (poll(&p, 1, timeout < 0 ? -1 : (int)timeout) < 0 && errno == EINTR) {
  }
  char bytes[64];
  while (read(tz_io_pipe[0], bytes, sizeof bytes) > 0) {
  }
  pthread_mutex_lock(&tz_io_lock);
  TzWork *back = tz_io_back_head;
  tz_io_back_head = tz_io_back_tail = NULL;
  pthread_mutex_unlock(&tz_io_lock);
  while (back) {
    TzWork *w = back;
    back = w->next;
    w->next = NULL;
    w->due = w->queued + (w->turns ? w->turns : 1);
    if (tz_io_due_tail) {
      tz_io_due_tail->next = w;
    } else {
      tz_io_due_head = w;
    }
    tz_io_due_tail = w;
  }
}

#endif

void tz_work_queue(TzWork *w) {
  w->next = NULL;
  w->settles = NULL;
  w->queued = tz_io_polls;
  if (!w->unref) {
    PENDING_ADD(1);
  }
  if (!w->work) {
    w->due = w->queued + (w->turns ? w->turns : 1);
    if (tz_io_due_tail) {
      tz_io_due_tail->next = w;
    } else {
      tz_io_due_head = w;
    }
    tz_io_due_tail = w;
    return;
  }
  if (!tz_io_threads) {
#ifdef _WIN32
    if (!tz_io_port) {
      start_port();
    }
#else
    if (tz_io_pipe[0] < 0) {
      start_port();
    }
#endif
    start_pool();
  }
  push_work(w);
}

void tz_work_queue_settles(TzWork *w, TzPromise *p) {
  tz_work_queue(w);
  w->settles = p;
}

void tz_io_begin(TzWork *w) {
  w->next = NULL;
  w->settles = NULL;
#if TZ_PARALLEL
  w->queued = __atomic_load_n(&tz_io_polls, __ATOMIC_RELAXED);
#else
  w->queued = tz_io_polls;
#endif
  if (!w->unref) {
    PENDING_ADD(1);
  }
}

#if TZ_PARALLEL && defined(_WIN32)
void tz_io_wake_loop(void);

void tz_io_due_remote(TzWork *w) {
  w->next = NULL;
  AcquireSRWLockExclusive(&tz_io_remote_lock);
  if (tz_io_remote_tail) {
    tz_io_remote_tail->next = w;
  } else {
    tz_io_remote_head = w;
  }
  tz_io_remote_tail = w;
  ReleaseSRWLockExclusive(&tz_io_remote_lock);
  tz_io_wake_loop();
}

void tz_io_done_remote(TzWork *w) {
  if (!w->unref && PENDING_ADD(-1) == 0) {
    tz_io_wake_loop();
  }
}
#endif

void tz_io_due(TzWork *w) {
  w->next = NULL;
  w->due = w->queued + (w->turns ? w->turns : 1);
  if (tz_io_due_tail) {
    tz_io_due_tail->next = w;
  } else {
    tz_io_due_head = w;
  }
  tz_io_due_tail = w;
}

#ifdef _WIN32
void *tz_io_port_handle(void) {
  if (!tz_io_port) {
    start_port();
  }

  return tz_io_port;
}
#endif

void tz_work_ref_count(int32_t n) { PENDING_ADD(n); }

void tz_work_ref(TzWork *w) {
  if (!w->unref) {
    return;
  }
  w->unref = 0;
  PENDING_ADD(1);
}

void tz_work_unref(TzWork *w) {
  if (w->unref) {
    return;
  }
  w->unref = 1;
  PENDING_ADD(-1);
}

int tz_io_alive(void) { return PENDING() > 0; }

#if TZ_PARALLEL
int32_t tz_ahead_defer(TzPromise *p);
int32_t tz_ahead_deliver(void);
extern int32_t tz_ahead_on;
void tz_trigger_hold(void);
TzPromise *tz_trigger_release(int32_t queue);

static void done_holding(TzWork *w) {
  tz_trigger_hold();
  w->done(w);
  TzPromise *one = tz_trigger_release(0);
  if (one) {
    if (!tz_ahead_defer(one)) {
      tz_promise_trigger(one);
    }
    tz_obj_release((TzObject *)one);
  }
}
#endif

void tz_io_poll(int64_t timeout) {
#if TZ_PARALLEL
  uint64_t poll = __atomic_add_fetch(&tz_io_polls, 1, __ATOMIC_RELAXED);
#else
  uint64_t poll = ++tz_io_polls;
#endif
  if (tz_io_due_head || (timeout < 0 && !PENDING())) {
    timeout = 0;
  }
  if (timeout != 0) {
    tz_out_idle();
  }
#if TZ_PARALLEL
  if (tz_ahead_on) {
    tz_ahead_deliver();
  }
#endif
  gather(timeout);
  TzWork *list = tz_io_due_head;
  tz_io_due_head = tz_io_due_tail = NULL;
  while (list) {
    TzWork *w = list;
    list = w->next;
    w->next = NULL;
    if (w->due > poll) {
      if (tz_io_due_tail) {
        tz_io_due_tail->next = w;
      } else {
        tz_io_due_head = w;
      }
      tz_io_due_tail = w;
      continue;
    }
    if (!w->unref) {
      PENDING_ADD(-1);
    }
#if TZ_PARALLEL
    if (tz_ahead_on) {
      done_holding(w);
      tz_after_callback();
      continue;
    }
#endif
    w->done(w);
    tz_after_callback();
  }
#if TZ_PARALLEL
  if (tz_ahead_on) {
    tz_ahead_deliver();
  }
#endif
}

#ifdef _WIN32
static void woken(OVERLAPPED_ENTRY *entry) { (void)entry; }

void tz_io_wake_loop(void) {
  if (tz_io_port) {
    PostQueuedCompletionStatus(tz_io_port, 0, (ULONG_PTR)woken, NULL);
  }
}
#else
void tz_io_wake_loop(void) {}
#endif
