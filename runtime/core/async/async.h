// Promises, microtasks, timers and the event loop.

#ifndef TZ_ASYNC_H
#define TZ_ASYNC_H

#include "runtime/core/closure/closure.h"
#include "runtime/core/tzrt/tzrt.h"

// Must match Lowering (compiler/lower/codegen/codegen_async.cpp).
enum { TZ_PROMISE_PENDING = 0, TZ_PROMISE_FULFILLED = 1, TZ_PROMISE_REJECTED = 2 };

typedef struct TzPromise TzPromise;
typedef struct TzReactor TzReactor;

// Something that waits for its turn in the microtask queue: a reaction to a
// promise, or a job. `run` gets the promise that settled (NULL for a job);
// after it returns, the runtime releases that promise, which the reactor
// held while queued, and `owner`, the object the reactor lives in. A reactor
// is in one list at a time, linked by `next`.
//
// `ahead`: when the piece of async code it goes on with may run ahead of
// its turn on a thread of the parallel runtime, the number of the await it
// goes on from (Lowering sets it at each await, from the decisions of
// compiler/parallel/parallel_analysis/parallel_analysis.h), 0 when not; once a thread took it on,
// the TzJob it runs as (parallel.c). Every other reactor has 0.
struct TzReactor {
  TzReactor *next;
  void (*run)(TzReactor *self, TzPromise *source);
  TzObject *owner;
  TzPromise *source;
  uintptr_t ahead;
};

// A promise: an object whose layout has TZ_LAYOUT_PROMISE and, unless it is
// a Promise<void>, one field, its value at TZ_PROMISE_VALUE, which gives the
// type of the value. The reactions wait in a list until it settles. The
// promise of an async function is followed by the reactor its coroutine
// awaits with and then by the frame of the coroutine.
struct TzPromise {
  int64_t rc;
  const TzLayout *layout;
  TzReactor *head, *tail;
  void *error; // the Error, once rejected
  uint32_t state;
  uint32_t flags;
};

#define TZ_PROMISE_VALUE 48

// Promise.all, allSettled, race and any (tz_promise_combine): the promise
// it gives, and where the values go: an array of `items`, or the tuple
// `tuple` (of `tuple_size` bytes) whose fields are the values in order. For
// allSettled, the results `{ status, value?, reason? }` that the array holds,
// with the type of `value` (`T | undefined`, or NULL for a Promise<void>).
enum { TZ_COMBINE_ALL = 0, TZ_COMBINE_ALL_SETTLED = 1, TZ_COMBINE_RACE = 2, TZ_COMBINE_ANY = 3 };

typedef struct TzCombine {
  const TzLayout *result;
  const TzType *items;
  const TzLayout *tuple;
  uint32_t tuple_size;
  uint32_t settled_size;
  const TzLayout *settled;
  const TzType *value_type;
  uint32_t status_offset, value_offset, reason_offset;
  // A tuple of promises whose values differ: race and any copy the value of
  // promise i into the union they give with convert[i] (NULL: as it is);
  // allSettled makes the result of promise i by each[i] (its settled_*,
  // value_type and offsets), into a tuple of them.
  void (*const *convert)(void *dst, const void *src);
  const struct TzCombine *each;
} TzCombine;

// setTimeout, setInterval and setImmediate: an object the program may keep,
// to clear it. A list of timers (or the queue of immediates) holds it while
// it waits.
typedef struct TzTimer {
  int64_t rc;
  const TzLayout *layout;
  TzFunction callback;
  struct TzTimer *prev, *next;
  struct TimerList *list;
  int64_t idle_start;
  double idle_timeout; // the delay; -1 once cleared
  double repeat;       // the delay of setInterval, else 0
  uint8_t refed, destroyed, immediate, cleared;
} TzTimer;

void tz_microtask(TzReactor *r);
void tz_run_microtasks(void);
// Its reactions to the microtask queue, as a promise that settles queues
// them (for io.c, which held them back).
void tz_promise_trigger(TzPromise *p);

void tz_promise_drop(void *promise);
void tz_promise_fulfill(TzPromise *p);
void tz_promise_reject(TzPromise *p, void *error);
TzPromise *tz_promise_new(const TzLayout *layout);
// `await p` (null: of a value that is no promise) in an async function: its
// reactor `r` is queued once p settles; `ahead`, the number of the await
// when the piece after it may run ahead of its turn (see TzReactor).
void tz_async_await(TzReactor *r, TzPromise *p, int32_t ahead);
// The nextTick callbacks and the microtasks, after a callback of the loop.
void tz_after_callback(void);
// A Promise<void> of the runtime's own.
extern const TzLayout tz_promise_void_layout;

// A promise of the values of an async iterator (for await, docs/iterators.md)
// fulfilled with this flag holds none: the iterator is done.
#define TZ_PROMISE_DONE 8

// An async iterator (docs/iterators.md, step 4): an object whose layout,
// flagged TZ_LAYOUT_ASYNC_ITERATOR, says how it goes on with what `for
// await` asks of it. next(it, p) settles p, a promise of the type of the
// values, with the next one, or fulfills it with TZ_PROMISE_DONE;
// close(it, p) is return(), which settles p, a Promise<void>, once the
// iterator is closed.
typedef struct TzAsyncIterLayout {
  TzLayout base;
  void (*next)(void *it, TzPromise *p);
  void (*close)(void *it, TzPromise *p);
} TzAsyncIterLayout;

// An async generator: the steps asked of it, its state, and the reactor its
// coroutine awaits with, which its frame follows. Must match Lowering
// (compiler/lower/codegen/codegen_async_iter.cpp).
typedef struct TzAgenStep {
  struct TzAgenStep *next;
  TzPromise *promise; // held
  int32_t close;      // return() rather than next()
} TzAgenStep;

typedef struct TzAsyncGen {
  int64_t rc;
  const TzLayout *layout; // a TzAsyncIterLayout
  TzAgenStep *head, *tail;
  int32_t state;
  int32_t reserved;
  TzReactor reactor;
} TzAsyncGen;

// Timers of the runtime's own (async.c).
TzTimer *tz_timer_new(const TzFunctionInfo *info, TzObject *env, double after, int32_t repeat);
void tz_timer_ref(TzTimer *t, int32_t ref);
void tz_timer_clear(TzTimer *t);
TzTimer *tz_immediate_new(const TzFunctionInfo *info, TzObject *env);
void tz_immediate_clear(TzTimer *t);

#endif
