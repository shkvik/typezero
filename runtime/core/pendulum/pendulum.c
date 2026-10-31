#include "runtime/core/pendulum/pendulum.h"
#include "runtime/core/async/async.h"
#include "runtime/core/output/output.h"
#include "runtime/core/parallel/parallel.h"
#include "runtime/core/tzrt/tzrt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if TZ_PARALLEL && defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

#define WEIGHTS 16
#define SLOTS 1024

typedef struct Slot {
  TzLock lock;
  int64_t since;
  int32_t count;
  TzWeight *weights[WEIGHTS];
} Slot;

TZ_THREAD_LOCAL Slot *tz_pendulum_slot;

#if TZ_PARALLEL
void tz_trigger_hold(void);
int32_t tz_trigger_holding(void);
TzPromise *tz_trigger_release(int32_t queue);
TzPromise **tz_trigger_take(int32_t *count);
void tz_ahead_forward(TzPromise *p);

Slot *tz_pendulum_slots[SLOTS];
int32_t tz_pendulum_watching;
int64_t tz_pendulum_wakes, tz_pendulum_steals, tz_pendulum_stolen;
int64_t tz_pendulum_intervals[6];

static int64_t now_ticks(void) {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);

  return now.QuadPart;
}

static int32_t take_weights(Slot *slot, TzWeight **taken) {
  tz_lock(&slot->lock);
  int32_t count = slot->count;
  memcpy(taken, slot->weights, (size_t)count * sizeof *taken);
  __atomic_store_n(&slot->count, 0, __ATOMIC_RELAXED);
  __atomic_store_n(&slot->since, 0, __ATOMIC_RELEASE);
  tz_unlock(&slot->lock);

  return count;
}

static void steal(Slot *slot) {
  TzWeight *taken[WEIGHTS];
  int32_t count = take_weights(slot, taken);
  if (!count) {
    return;
  }
  ++tz_pendulum_steals;
  tz_pendulum_stolen += count;
  tz_trigger_hold();
  for (int32_t i = 0; i < count; ++i) {
    taken[i]->release(taken[i]);
  }
  int32_t held_count;
  TzPromise **held = tz_trigger_take(&held_count);
  for (int32_t i = 0; i < held_count; ++i) {
    tz_ahead_forward(held[i]);
  }
}

static void print_watch(void) {
  fprintf(stderr,
          "pendulum: watchdog woke %lld times (asked 0.5 ms; got <=0.6 %lld, <=1 %lld, <=2 %lld, "
          "<=5 %lld, <=16 %lld, more %lld), stole %lld times, %lld weights\n",
          (long long)tz_pendulum_wakes, (long long)tz_pendulum_intervals[0],
          (long long)tz_pendulum_intervals[1], (long long)tz_pendulum_intervals[2],
          (long long)tz_pendulum_intervals[3], (long long)tz_pendulum_intervals[4],
          (long long)tz_pendulum_intervals[5], (long long)tz_pendulum_steals,
          (long long)tz_pendulum_stolen);
}

static DWORD WINAPI watchdog(void *arg) {
  (void)arg;
  tz_thread_register();
  tz_thread_stack_init();
  tz_thread.worker = 1;
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  HANDLE timer =
      CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!timer) {
    timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
  }
  LARGE_INTEGER frequency;
  QueryPerformanceFrequency(&frequency);
  int64_t threshold = frequency.QuadPart / 1000;
  int64_t tenth = frequency.QuadPart / 10000;
  int64_t last = now_ticks();
  for (;;) {
    LARGE_INTEGER due;
    due.QuadPart = -5000;
    SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
    WaitForSingleObject(timer, INFINITE);
    int64_t now = now_ticks();
    int64_t interval = now - last;
    last = now;
    ++tz_pendulum_wakes;
    int bucket = interval <= 6 * tenth     ? 0
                 : interval <= 10 * tenth  ? 1
                 : interval <= 20 * tenth  ? 2
                 : interval <= 50 * tenth  ? 3
                 : interval <= 160 * tenth ? 4
                                           : 5;
    ++tz_pendulum_intervals[bucket];
    int32_t total = tz_thread_total();
    for (int32_t i = 0; i < total && i < SLOTS; ++i) {
      Slot *slot = __atomic_load_n(&tz_pendulum_slots[i], __ATOMIC_ACQUIRE);
      if (!slot) {
        continue;
      }
      int64_t since = __atomic_load_n(&slot->since, __ATOMIC_ACQUIRE);
      if (since && now - since >= threshold) {
        steal(slot);
      }
    }
  }

  return 0;
}

static void start_watchdog(void) {
  int32_t none = 0;
  if (!__atomic_compare_exchange_n(&tz_pendulum_watching, &none, 1, 0, __ATOMIC_ACQ_REL,
                                   __ATOMIC_RELAXED)) {
    return;
  }
  if (getenv("TZ_PARALLEL_STATS")) {
    atexit(print_watch);
  }
  HANDLE thread = CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
  if (thread) {
    CloseHandle(thread);
  }
}
#endif

static Slot *own_slot(void) {
  Slot *slot = tz_pendulum_slot;
  if (slot) {
    return slot;
  }
  slot = (Slot *)calloc(1, sizeof(Slot));
  if (!slot) {
    tz_panic("out of memory", 13);
  }
  tz_pendulum_slot = slot;
#if TZ_PARALLEL
  int32_t index = tz_thread.index;
  if (index >= 0 && index < SLOTS) {
    __atomic_store_n(&tz_pendulum_slots[index], slot, __ATOMIC_RELEASE);
  }
  start_watchdog();
#endif

  return slot;
}

TzHold tz_pendulum_hold(TzWeight *weight) {
  Slot *slot = own_slot();
  tz_lock(&slot->lock);
  int32_t count = slot->count;
  for (int32_t i = 0; i < count; ++i) {
    if (slot->weights[i] == weight) {
      tz_unlock(&slot->lock);
      return TZ_HOLD_ALREADY;
    }
  }
  if (count == WEIGHTS) {
    tz_unlock(&slot->lock);
    return TZ_HOLD_FULL;
  }
  slot->weights[count] = weight;
  __atomic_store_n(&slot->count, count + 1, __ATOMIC_RELAXED);
#if TZ_PARALLEL
  if (!count) {
    __atomic_store_n(&slot->since, now_ticks(), __ATOMIC_RELEASE);
  }
#endif
  tz_unlock(&slot->lock);

  return TZ_HOLD_NEW;
}

int32_t tz_pendulum_holding(void) {
  Slot *slot = tz_pendulum_slot;

  return slot ? __atomic_load_n(&slot->count, __ATOMIC_RELAXED) : 0;
}

void tz_pendulum_swing(void) {
  Slot *slot = tz_pendulum_slot;
  if (!slot || !__atomic_load_n(&slot->count, __ATOMIC_RELAXED)) {
    return;
  }
  TzWeight *taken[WEIGHTS];
#if TZ_PARALLEL
  int32_t count = take_weights(slot, taken);
  if (!count) {
    return;
  }
  int holding = tz_trigger_holding();
  if (!holding) {
    tz_trigger_hold();
  }
#else
  int32_t count = slot->count;
  memcpy(taken, slot->weights, (size_t)count * sizeof *taken);
  slot->count = 0;
#endif
  for (int32_t i = 0; i < count; ++i) {
    taken[i]->release(taken[i]);
  }
#if TZ_PARALLEL
  if (holding) {
    return;
  }
  if (tz_thread.worker && !tz_thread.job) {
    tz_ahead_settle_held();
  } else {
    tz_trigger_release(1);
  }
#endif
}
