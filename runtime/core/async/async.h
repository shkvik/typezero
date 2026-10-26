#ifndef TZ_ASYNC_H
#define TZ_ASYNC_H

#include "runtime/core/closure/closure.h"
#include "runtime/core/tzrt/tzrt.h"

enum { TZ_PROMISE_PENDING = 0, TZ_PROMISE_FULFILLED = 1, TZ_PROMISE_REJECTED = 2 };

typedef struct TzPromise TzPromise;
typedef struct TzReactor TzReactor;

struct TzReactor {
  TzReactor *next;
  void (*run)(TzReactor *self, TzPromise *source);
  TzObject *owner;
  TzPromise *source;
  uintptr_t ahead;
};

struct TzPromise {
  int64_t rc;
  const TzLayout *layout;
  TzReactor *head, *tail;
  void *error;
  uint32_t state;
  uint32_t flags;
};

#define TZ_PROMISE_VALUE 48

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
  void (*const *convert)(void *dst, const void *src);
  const struct TzCombine *each;
} TzCombine;

typedef struct TzTimer {
  int64_t rc;
  const TzLayout *layout;
  TzFunction callback;
  struct TzTimer *prev, *next;
  struct TimerList *list;
  int64_t idle_start;
  double idle_timeout;
  double repeat;
  uint8_t refed, destroyed, immediate, cleared;
} TzTimer;

void tz_microtask(TzReactor *r);
void tz_run_microtasks(void);
void tz_promise_trigger(TzPromise *p);

void tz_promise_drop(void *promise);
void tz_promise_fulfill(TzPromise *p);
void tz_promise_reject(TzPromise *p, void *error);
TzPromise *tz_promise_new(const TzLayout *layout);
void tz_async_await(TzReactor *r, TzPromise *p, int32_t ahead);
void tz_after_callback(void);
extern const TzLayout tz_promise_void_layout;

#define TZ_PROMISE_DONE 8

typedef struct TzAsyncIterLayout {
  TzLayout base;
  void (*next)(void *it, TzPromise *p);
  void (*close)(void *it, TzPromise *p);
} TzAsyncIterLayout;

typedef struct TzAgenStep {
  struct TzAgenStep *next;
  TzPromise *promise;
  int32_t close;
} TzAgenStep;

typedef struct TzAsyncGen {
  int64_t rc;
  const TzLayout *layout;
  TzAgenStep *head, *tail;
  int32_t state;
  int32_t reserved;
  TzReactor reactor;
} TzAsyncGen;

TzTimer *tz_timer_new(const TzFunctionInfo *info, TzObject *env, double after, int32_t repeat);
void tz_timer_ref(TzTimer *t, int32_t ref);
void tz_timer_clear(TzTimer *t);
TzTimer *tz_immediate_new(const TzFunctionInfo *info, TzObject *env);
void tz_immediate_clear(TzTimer *t);

#endif
