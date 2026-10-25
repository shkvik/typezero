// Promises, the microtask queue, process.nextTick, timers, setImmediate and
// the event loop (docs/async.md, step 1): one thread, in the order Node runs
// things.
//
// After the top-level code of the program, and after each callback of the
// loop (a timer, an immediate), Node runs the process.nextTick callbacks,
// then the microtask queue until it is empty, again while either has
// something, and then reports a rejected promise that nothing handles. (The
// top-level code of an ES module, a file with `import`, `export` or
// top-level `await`, runs inside a microtask itself, so there the
// microtasks come first.) The loop is libuv's:
// timers, the poll phase that waits for I/O until the next timer is due
// (io.c), the immediates, the timers again, while a timer, an immediate or
// a work of the thread pool that is not unref()'d keeps it alive. Timers are
// kept as Node keeps them
// (lib/internal/timers.js): a list for each duration, in the order they were
// set, and the lists ordered by when their first timer expires. That order,
// rather than when each timer expires, decides which runs first.
//
// A promise keeps its reactions in a list until it settles; then they move
// to the end of the microtask queue, each holding the promise while it waits
// there. A reaction is a TzReactor inside some object: the coroutine of an
// async function (compiler/lower/codegen/codegen_async.cpp), the reaction of a
// then(), whose code the compiler makes, or one of the runtime's own below. The
// jobs of the runtime take as many turns of the queue as V8's do, so that the
// output interleaves as in Node.

#include "runtime/core/tzrt/tzrt.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

TZ_NORETURN void tz_uncaught(void);
extern TzErrorInfo tz_error_info;
void tz_err_begin(void);
void tz_err_end(void);
double tz_performance_now(void);

// The flags of a promise. HANDLED: a reaction was attached to it, now or
// before, so its rejection is not reported. LOCKED: its resolve functions
// were used. LISTED: it waits in the list of rejections to check.
enum { HANDLED = 1, LOCKED = 2, LISTED = 4 };

static void retain(void *object) { tz_ref_retain(object); }

static void release(void *object) { tz_obj_release((TzObject *)object); }

// What a callback the program gave (`() => void`) runs. An error it throws
// that nothing catches ends the program, as an uncaught exception in Node.
static void call(TzFunction fn) {
  ((void (*)(TzObject *))fn.info->code)(fn.env);
  if (tz_thrown) {
    tz_uncaught();
  }
}

//===----------------------------------------------------------------------===//
// The microtask queue
//===----------------------------------------------------------------------===//

// None of the state is static: see the rules in tzrt.h.
TzReactor *tz_micro_head, *tz_micro_tail;

#if TZ_PARALLEL
// parallel.c: a piece that runs ahead of the event of I/O it goes on from
// (a job of its own, early) keeps the microtasks it queues while it has no
// turn, if what they go on with may run ahead too, and runs them after it
// (1); not otherwise (0). Whether only the thread that calls it can reach
// `object`.
int32_t tz_ahead_local(TzReactor *r);
int32_t tz_ahead_early(void);
int32_t tz_ahead_private(void *object);
#endif

void tz_microtask(TzReactor *r) {
#if TZ_PARALLEL
  if (!tz_thread.turn && tz_ahead_local(r)) {
    return;
  }
#endif
  TZ_ORDERED();
  r->next = NULL;
  if (tz_micro_tail) {
    tz_micro_tail->next = r;
  } else {
    tz_micro_head = r;
  }
  tz_micro_tail = r;
}

#if TZ_PARALLEL
// parallel.c: the reactors behind `head` that may run ahead of their turn
// go to the threads of the pool; one that may runs as tz_ahead_run says:
// with its turn on the thread that took it, or here. It releases the source.
void tz_ahead_dispatch(TzReactor *head);
void tz_ahead_run(TzReactor *r, TzPromise *source);
void tz_ahead_popped(TzReactor *r);
int32_t tz_ahead_keep(int32_t site);
extern int32_t tz_ahead_on; // the pool has threads (TZ_THREADS)
#endif

// Runs the jobs, those they queue included, until there are none.
void tz_run_microtasks(void) {
  while (tz_micro_head) {
    TzReactor *r = tz_micro_head;
#if TZ_PARALLEL
    if (tz_ahead_on) {
      if (r->next) {
        tz_ahead_dispatch(r);
      }
      tz_ahead_popped(r);
    }
#endif
    tz_micro_head = r->next;
    if (!tz_micro_head) {
      tz_micro_tail = NULL;
    }
    // The job may queue its reactor again (an async function that awaits
    // once more), which sets these anew.
    TzPromise *source = r->source;
    TzObject *owner = r->owner;
    r->source = NULL;
#if TZ_PARALLEL
    // One that may run ahead of its turn: on the thread that took it, with
    // the main thread's turn, or here, timed.
    if (r->ahead && tz_ahead_on) {
      tz_ahead_run(r, source);
      tz_obj_release(owner);
      continue;
    }
#endif
    r->run(r, source);
    if (source) {
      release(source);
    }
    tz_obj_release(owner);
  }
}

//===----------------------------------------------------------------------===//
// Promises
//===----------------------------------------------------------------------===//

// The rejected promises without a handler, checked once the microtasks are
// done (processPromiseRejections in Node). Each is held while listed.
TzPromise **tz_rejections;
size_t tz_rejection_count, tz_rejection_capacity;

static const TzType *value_type(const TzPromise *p) {
  return p->layout->count ? p->layout->fields[0].type : NULL;
}

static void *value_of(const TzPromise *p) { return (char *)p + TZ_PROMISE_VALUE; }

// A promise whose value or error may be a member of a group is one itself
// (TZ_LAYOUT_GROUP, which CodeGen::promiseLayout sets): `resolve` of `new
// Promise`, whose environment is the promise, may be held inside the group
// of what it settles the promise with, and a promise outside it would count
// that from outside, so that neither could ever be freed.
TzPromise *tz_promise_new(const TzLayout *layout) {
  const TzType *type = layout->count ? layout->fields[0].type : NULL;
  size_t size = TZ_PROMISE_VALUE + (type ? type->size : 0);
  if (layout->flags & TZ_LAYOUT_GROUP) {
    TzPromise *p = (TzPromise *)tz_group_alloc((int64_t)size);
    int64_t rc = p->rc;
    memset(p, 0, size);
    p->rc = rc;
    p->layout = layout;
    return p;
  }
  TzPromise *p = (TzPromise *)tz_obj_alloc((int64_t)size);
  memset(p, 0, size);
  p->rc = 1;
  p->layout = layout;

  return p;
}

// The type of the error of a rejected promise, for releasing it as a member
// does.
static const TzType error_type = {TZ_T_OBJECT, sizeof(void *), NULL};

// The layout's drop function, for every promise: an async function's too,
// whose coroutine has released what its frame held by the time its promise
// can be dropped.
void tz_promise_drop(void *object) {
  TzPromise *p = (TzPromise *)object;
  const TzType *type = value_type(p);
  // A member lets go of what it settled with as a member does.
  int member = p->rc < 0;
  if (p->state == TZ_PROMISE_FULFILLED) {
    if (type && member) {
      tz_value_release_in(p, type, value_of(p));
    } else if (type) {
      tz_value_release(type, value_of(p));
    }
  } else if (p->state == TZ_PROMISE_REJECTED) {
    if (member && p->error) {
      tz_value_release_in(p, &error_type, &p->error);
    } else {
      release(p->error);
    }
  }
  // Reactions to a promise that can no longer settle never run. A member
  // holds them as a member holds what is in its fields (tz_promise_react).
  for (TzReactor *r = p->head; r;) {
    TzReactor *next = r->next;
    if (member && r->owner->rc < 0) {
      tz_group_unstore(p, r->owner);
    } else {
      tz_obj_release(r->owner);
    }
    r = next;
  }
  if (member) {
    tz_group_obj_free(p);
  } else {
    tz_obj_free(p);
  }
}

#if TZ_PARALLEL
// io.c: while an event's callback runs, the promises it settles keep their
// reactions (held, each retained), until tz_trigger_release queues them in
// the same order: the main thread in the poll phase, and a thread of the
// pool as it takes what came for a connection (net.c), which then hands
// them on (tz_trigger_take). Each thread its own.
#define HELD_MAX 16
TZ_THREAD_LOCAL TzPromise *tz_held[HELD_MAX];
TZ_THREAD_LOCAL int32_t tz_held_count = -1; // -1: nothing held

static void trigger(TzPromise *p);

void tz_trigger_hold(void) { tz_held_count = 0; }

// Queues what was held; with `queue` 0 and one promise held, gives it
// (still held by the caller) without queueing its reactions.
TzPromise *tz_trigger_release(int32_t queue) {
  int32_t n = tz_held_count;
  tz_held_count = -1;
  if (n == 1 && !queue) {
    return tz_held[0];
  }
  for (int32_t i = 0; i < n; ++i) {
    trigger(tz_held[i]);
    release(tz_held[i]);
  }

  return NULL;
}

// What was held, at `out` (HELD_MAX of them at most), each still retained:
// how many.
int32_t tz_trigger_take(TzPromise **out) {
  int32_t n = tz_held_count;
  tz_held_count = -1;
  for (int32_t i = 0; i < n; ++i) {
    out[i] = tz_held[i];
  }

  return n < 0 ? 0 : n;
}
#endif

static void trigger(TzPromise *p);

void tz_promise_trigger(TzPromise *p) { trigger(p); }

// Its reactions go to the end of the microtask queue, in the order they
// were attached.
static void trigger(TzPromise *p) {
#if TZ_PARALLEL
  if (tz_held_count >= 0 && p->head) {
    if (tz_held_count == HELD_MAX) {
      // Too many: those held so far go first, in order.
      TZ_ORDERED();
      tz_held_count = -1;
      for (int32_t i = 0; i < HELD_MAX; ++i) {
        trigger(tz_held[i]);
        release(tz_held[i]);
      }
      tz_held_count = 0;
    }
    retain(p);
    tz_held[tz_held_count++] = p;
    return;
  }
#endif
  TzReactor *head = p->head;
  if (!head) {
    return;
  }
#if TZ_PARALLEL
  if (!tz_thread.turn) {
    // In a piece ahead of its event, settling what may run ahead with it
    // (tz_promise_fulfill): the reactions go on after it, on its thread.
    p->head = p->tail = NULL;
    for (TzReactor *r = head; r;) {
      TzReactor *next = r->next;
      r->source = p;
      retain(p);
      if (p->rc < 0 && r->owner->rc < 0) {
        tz_group_take(p, r->owner);
      }
      if (!tz_ahead_local(r)) {
        tz_microtask(r);
      }
      r = next;
    }
    return;
  }
#endif
  for (TzReactor *r = head; r; r = r->next) {
    r->source = p;
    retain(p);
    // The queue holds the reaction now, counted (see tz_promise_react).
    if (p->rc < 0 && r->owner->rc < 0) {
      tz_group_take(p, r->owner);
    }
  }
  if (tz_micro_tail) {
    tz_micro_tail->next = head;
  } else {
    tz_micro_head = head;
  }
  tz_micro_tail = p->tail;
  p->head = p->tail = NULL;
}

#if TZ_PARALLEL
// Whether all that waits for `p` may run ahead in the piece that settles it,
// one ahead of its event (parallel.c): their microtasks stay with it.
static int reactions_ahead(const TzPromise *p) {
  if (!tz_ahead_early()) {
    return 0;
  }
  for (const TzReactor *r = p->head; r; r = r->next) {
    uintptr_t ahead = r->ahead;
    if (!ahead || ahead >= ((uintptr_t)1 << 20)) { // see MAX_SITE in parallel.c
      return 0;
    }
  }

  return 1;
}
#endif

void tz_promise_fulfill(TzPromise *p) {
#if TZ_PARALLEL
  // Nobody can tell when a promise settled that only this thread sees, if
  // nothing waits for it yet or what does may run ahead here, or whose
  // reactions wait to be handed on.
  if (!tz_thread.turn && tz_held_count < 0 &&
      !(tz_ahead_private(p) && (!p->head || reactions_ahead(p)))) {
    tz_order_wait();
  }
#endif
  p->state = TZ_PROMISE_FULFILLED;
  // What it holds now is inside its group.
  const TzType *type = value_type(p);
#if TZ_PARALLEL
  // Its awaiters may go on with it on other threads (publish.c); a promise
  // done as an iterator holds none.
  if (type && !(p->flags & TZ_PROMISE_DONE)) {
    tz_publish_value(type, value_of(p));
  }
#endif
  if (p->rc < 0 && type) {
    tz_group_store_value(p, type, value_of(p));
  }
  trigger(p);
}

void tz_promise_reject(TzPromise *p, void *error) {
  TZ_ORDERED();
  p->state = TZ_PROMISE_REJECTED;
  p->error = error;
#if TZ_PARALLEL
  tz_publish_object(error);
#endif
  if (p->rc < 0 && error) {
    tz_group_store(p, error);
  }
  if (!(p->flags & (HANDLED | LISTED))) {
    if (tz_rejection_count == tz_rejection_capacity) {
      size_t capacity = tz_rejection_capacity ? tz_rejection_capacity * 2 : 8;
      TzPromise **grown = (TzPromise **)tz_alloc(capacity * sizeof *grown);
      if (tz_rejection_count) {
        memcpy(grown, tz_rejections, tz_rejection_count * sizeof *grown);
      }
      tz_free(tz_rejections);
      tz_rejections = grown;
      tz_rejection_capacity = capacity;
    }
    p->flags |= LISTED;
    retain(p);
    tz_rejections[tz_rejection_count++] = p;
  }
  trigger(p);
}

int32_t tz_promise_lock(TzPromise *p) {
  TZ_ORDERED();
  if (p->flags & LOCKED) {
    return 0;
  }
  p->flags |= LOCKED;

  return 1;
}

void tz_promise_react(TzPromise *p, TzReactor *r) {
  TZ_ORDERED();
  p->flags |= HANDLED;
  if (p->state == TZ_PROMISE_PENDING) {
    r->next = NULL;
    if (p->tail) {
      p->tail->next = r;
    } else {
      p->head = r;
    }
    p->tail = r;
    // The reference to what the reaction lives in is the promise's own now:
    // inside its group when both are members, as a store into a field.
    if (p->rc < 0) {
      tz_group_store(p, r->owner);
    }
    return;
  }
  r->source = p;
  retain(p);
  tz_microtask(r);
}

#if TZ_PARALLEL
// The lock a function of the runtime took for the promise it gave
// (tz_thread.await_lock, net.c) goes once the await is set.
static void await_unlock(void) {
  TzLock *lock = (TzLock *)tz_thread.await_lock;
  if (!lock) {
    return;
  }
  tz_thread.await_lock = NULL;
  --tz_thread.conn_locks;
  tz_unlock(lock);
}

// A piece that runs ahead of its event (parallel.c) awaits without the turn
// what only it can see, when what goes on after the await may run ahead too:
// a promise that a function of the runtime just gave, its source locked
// until now, so that it cannot settle meanwhile; one no other thread can
// reach; or a value that is no promise. What is settled already goes on
// after this piece, on this thread.
static int await_ahead(TzReactor *r, TzPromise *p, int32_t ahead) {
  if (!ahead || !tz_ahead_early()) {
    return 0;
  }
  if (!p) {
    r->ahead = (uintptr_t)ahead;
    retain(r->owner);
    if (!tz_ahead_local(r)) {
      tz_microtask(r); // cannot be: the piece is early and the reaction may run ahead
    }
    return 1;
  }
  if (!tz_thread.await_lock && !tz_ahead_private(p)) {
    return 0;
  }
  if (p->state == TZ_PROMISE_PENDING) {
    r->ahead = (uintptr_t)ahead;
    retain(r->owner);
    p->flags |= HANDLED;
    r->next = NULL;
    if (p->tail) {
      p->tail->next = r;
    } else {
      p->head = r;
    }
    p->tail = r;
    if (p->rc < 0) {
      tz_group_store(p, r->owner);
    }
    await_unlock();
    return 1;
  }
  if (p->state != TZ_PROMISE_FULFILLED) {
    return 0;
  }
  await_unlock();
  r->ahead = (uintptr_t)ahead;
  retain(r->owner);
  p->flags |= HANDLED;
  r->source = p;
  retain(p);
  if (!tz_ahead_local(r)) {
    // Cannot be: the piece is early and the reaction may run ahead.
    tz_microtask(r);
  }

  return 1;
}
#endif

void tz_async_await(TzReactor *r, TzPromise *p, int32_t ahead) {
#if TZ_PARALLEL
  if (!tz_thread.turn && await_ahead(r, p, ahead)) {
    return;
  }
  // The source of the promise stays locked until the reaction is set, but
  // while this waits for the turn (tz_order_wait lets go of it).
  TzLock *source = (TzLock *)tz_thread.await_lock;
  if (source && !tz_thread.turn) {
    tz_order_wait();
    tz_lock(source);
    ++tz_thread.conn_locks;
    tz_thread.await_lock = source;
  }
#endif
  TZ_ORDERED();
  // Whether what comes after this await may run ahead of its turn
  // (parallel.c); set only now that this piece has its turn, since the
  // main thread looks at the reactors of the queue. A site whose pieces are
  // short has its reactors run as any other, but now and then.
#if TZ_PARALLEL
  r->ahead = ahead && tz_ahead_on ? (uintptr_t)tz_ahead_keep(ahead) : 0;
#else
  r->ahead = (uintptr_t)ahead;
#endif
  // The coroutine waits: its object stays while the reactor is queued.
  retain(r->owner);
  if (!p) {
    tz_microtask(r);
  } else {
    tz_promise_react(p, r);
  }
#if TZ_PARALLEL
  await_unlock();
#endif
}

// `target` settles as `source` did, with a copy of its value or its error.
static void settle_like(TzPromise *target, const TzPromise *source) {
  if (source->state == TZ_PROMISE_FULFILLED) {
    const TzType *type = value_type(target);
    if (type) {
      memcpy(value_of(target), value_of(source), type->size);
      tz_value_retain(type, value_of(target));
    }
    tz_promise_fulfill(target);
  } else {
    retain(source->error);
    tz_promise_reject(target, source->error);
  }
}

// A promise that follows another: `target` settles as `origin` does. First
// it takes a turn of the queue, as the job that calls origin.then() does in
// V8 (NewPromiseResolveThenableJob), then it waits for origin.
typedef struct {
  TzObject base;
  TzReactor reactor;
  TzPromise *target, *origin;
  // finally(): the promise the callback gave, which goes first; origin then
  // decides how target settles, unless that one is rejected.
  TzPromise *first;
} Follow;

static void follow_drop(void *object) {
  Follow *f = (Follow *)object;
  release(f->target);
  release(f->origin);
  if (f->first) {
    release(f->first);
  }
  tz_obj_free(f);
}

static const TzObjectDesc follow_desc = {follow_drop};

static void follow_settle(TzReactor *r, TzPromise *source) {
  Follow *f = (Follow *)((char *)r - offsetof(Follow, reactor));
  if (f->first && source->state == TZ_PROMISE_REJECTED) {
    settle_like(f->target, source);
  } else {
    settle_like(f->target, f->first ? f->origin : source);
  }
}

static void follow_then(TzReactor *r, TzPromise *source) {
  (void)source;
  Follow *f = (Follow *)((char *)r - offsetof(Follow, reactor));
  ++f->base.rc; // the reaction holds it now
  r->run = follow_settle;
  tz_promise_react(f->origin, r);
}

static Follow *new_follow(TzPromise *target, TzPromise *origin) {
  Follow *f = (Follow *)tz_obj_alloc(sizeof(Follow));
  f->base.rc = 1;
  f->base.desc = &follow_desc;
  f->reactor.next = NULL;
  f->reactor.owner = &f->base;
  f->reactor.source = NULL;
  f->reactor.ahead = 0;
  f->target = target;
  f->origin = origin;
  f->first = NULL;

  return f;
}

void tz_promise_follow(TzPromise *p, TzPromise *q) {
  TZ_ORDERED();
  retain(p);
  Follow *f = new_follow(p, q);
  f->reactor.run = follow_then;
  tz_microtask(&f->reactor);
}

// A Promise<void> of the runtime's own, for finally() with a callback that
// gives no promise.
const TzLayout tz_promise_void_layout = {tz_promise_drop, NULL, 0, TZ_LAYOUT_PROMISE, NULL, 0, 0};

void tz_promise_finally(TzPromise *derived, TzPromise *original, TzPromise *awaited) {
  TZ_ORDERED();
  // As the spec says: `PromiseResolve(result).then(() => value)`, which
  // derived then follows.
  if (!awaited) {
    awaited = tz_promise_new(&tz_promise_void_layout);
    awaited->state = TZ_PROMISE_FULFILLED;
  }
  TzPromise *next = tz_promise_new(original->layout);
  retain(next);
  retain(original);
  Follow *f = new_follow(next, original);
  f->first = awaited;
  f->reactor.run = follow_settle;
  tz_promise_react(awaited, &f->reactor);
  tz_promise_follow(derived, next);
}

// Promise.all and the like of values that are no promises: a resolved
// promise of each.
TzArray *tz_promise_resolve_all(const TzArray *values, const TzLayout *layout,
                                const TzType *promises) {
  uint32_t count = values->length;
  TzArray *out = tz_arr_new(promises, count, count);
  const TzType *type = layout->count ? layout->fields[0].type : NULL;
  for (uint32_t i = 0; i < count; ++i) {
    TzPromise *p = tz_promise_new(layout);
    p->state = TZ_PROMISE_FULFILLED;
    if (type) {
      memcpy(value_of(p), values->data + (size_t)i * type->size, type->size);
      tz_value_retain(type, value_of(p));
    }
    ((TzPromise **)out->data)[i] = p;
  }

  return out;
}

// What the program does a turn of the queue later: the turns the code of
// neverthrow's ResultAsync takes, which the program's own follows.
typedef struct {
  TzObject base;
  TzReactor reactor;
  TzPromise *promise;
  // The reaction that then waits for `promise`, or none to fulfill it.
  TzReactor *reaction;
} Later;

static void later_drop(void *object) {
  release(((Later *)object)->promise);
  tz_obj_free(object);
}

static const TzObjectDesc later_desc = {later_drop};

static void later_run(TzReactor *r, TzPromise *source) {
  (void)source;
  Later *later = (Later *)((char *)r - offsetof(Later, reactor));
  if (later->reaction) {
    tz_promise_react(later->promise, later->reaction);
  } else {
    tz_promise_fulfill(later->promise);
  }
}

static void queue_later(TzPromise *p, TzReactor *reaction) {
  TZ_ORDERED();
  Later *later = (Later *)tz_obj_alloc(sizeof(Later));
  later->base.rc = 1;
  later->base.desc = &later_desc;
  later->reactor.next = NULL;
  later->reactor.run = later_run;
  later->reactor.owner = &later->base;
  later->reactor.source = NULL;
  later->reactor.ahead = 0;
  later->promise = p;
  later->reaction = reaction;
  retain(p);
  tz_microtask(&later->reactor);
}

// Fulfills `p`, whose value the program stored already, a turn later: as
// an async function's promise is after `await` of a value.
void tz_promise_fulfill_later(TzPromise *p) { queue_later(p, NULL); }

// Attaches the reaction `r`, which the program owns no more, to `p` a turn
// later: a promise that follows `p` through a reaction that converts its
// value takes the turn of the job that calls then() too (see Follow).
void tz_promise_react_later(TzPromise *p, TzReactor *r) { queue_later(p, r); }

TzPromise *tz_promise_resolved(const TzLayout *layout) {
  TzPromise *p = tz_promise_new(layout);
  p->state = TZ_PROMISE_FULFILLED;

  return p;
}

TzPromise *tz_promise_rejected(const TzLayout *layout, void *error) {
  TZ_ORDERED();
  TzPromise *p = tz_promise_new(layout);
  tz_promise_reject(p, error);

  return p;
}

//===----------------------------------------------------------------------===//
// Promise.all, allSettled, race and any
//===----------------------------------------------------------------------===//

// What they collect, shared by the reactions to each promise.
typedef struct {
  TzObject base;
  TzPromise *result;
  void *container; // the array of values or results, or the tuple; owned until given
  int64_t remaining;
  int32_t kind;
  const TzCombine *shape;
} Combine;

typedef struct {
  TzObject base;
  TzReactor reactor;
  Combine *all;
  uint32_t index;
} Element;

static void combine_drop(void *object) {
  Combine *c = (Combine *)object;
  release(c->result);
  if (c->shape->tuple) {
    if (c->container) {
      release(c->container);
    }
  } else if (c->container) {
    TzArray *array = (TzArray *)c->container;
    if (tz_rc_is_member(array->rc)) { // a member of a group (group.c)
      tz_group_release(array);
    } else if (tz_rc_dec(&array->rc)) {
      tz_arr_free(array);
    }
  }
  tz_obj_free(c);
}

static const TzObjectDesc combine_desc = {combine_drop};

static void element_drop(void *object) {
  Element *e = (Element *)object;
  release(e->all);
  tz_obj_free(e);
}

static const TzObjectDesc element_desc = {element_drop};

// Where the value of element i goes in the container.
static char *slot_of(Combine *c, uint32_t i, const TzType **type) {
  const TzCombine *shape = c->shape;
  if (shape->tuple) {
    const TzField *field = &shape->tuple->fields[i];
    *type = field->type;
    return (char *)c->container + field->offset;
  }
  TzArray *array = (TzArray *)c->container;
  *type = array->type;

  return array->data + (size_t)i * array->type->size;
}

static TzStr ascii(const char *text) { return tz_str_from_ascii(text, strlen(text)); }

// `{ status: "fulfilled", value }` or `{ status: "rejected", reason }`.
static void *settled_result(const TzCombine *shape, const TzPromise *source) {
  char *object = (char *)tz_obj_alloc(shape->settled_size);
  memset(object, 0, shape->settled_size);
  ((TzInstance *)object)->rc = 1;
  ((TzInstance *)object)->layout = shape->settled;
  int fulfilled = source->state == TZ_PROMISE_FULFILLED;
  *(TzStr *)(object + shape->status_offset) = ascii(fulfilled ? "fulfilled" : "rejected");
  if (fulfilled) {
    // `T | undefined`: the value, then (unless zero is undefined for it: a
    // reference, a union, a string literal type; see tz_optional_has) the
    // byte that says it is there.
    const TzType *optional = shape->value_type;
    if (optional && optional->inner->size) {
      memcpy(object + shape->value_offset, value_of(source), optional->inner->size);
      tz_value_retain(optional->inner, object + shape->value_offset);
    }
    int kind = optional ? optional->inner->kind : 0;
    if (optional && kind != TZ_T_OBJECT && kind != TZ_T_JSON && kind != TZ_T_UNION &&
        kind != TZ_T_ATOM) {
      object[shape->value_offset + optional->inner->size] = 1;
    }
  } else {
    retain(source->error);
    *(void **)(object + shape->reason_offset) = source->error;
  }

  return object;
}

// Every value is in: the result gets the container.
static void combine_done(Combine *c) {
  if (c->kind == TZ_COMBINE_ANY) {
    // All were rejected, as an AggregateError says (without its `errors`,
    // which only Error's fields could not hold).
    TzStr message = ascii("All promises were rejected");
    void *error = tz_error_info.make(TZ_ERROR, message.obj, message.meta);
    TzStr *name = (TzStr *)((char *)error + tz_error_info.name_offset);
    tz_str_release_value(*name);
    *name = ascii("AggregateError");
    tz_promise_reject(c->result, error);
    return;
  }
  if (c->container) {
    *(void **)value_of(c->result) = c->container;
  }
  c->container = NULL;
  tz_promise_fulfill(c->result);
}

// Race and any: the result settles as promise i did, its value made one of
// the union the result holds when the values differ.
static void settle_from(Combine *c, uint32_t i, const TzPromise *source) {
  void (*convert)(void *, const void *) = c->shape->convert ? c->shape->convert[i] : NULL;
  if (convert && source->state == TZ_PROMISE_FULFILLED) {
    convert(value_of(c->result), value_of(source));
    tz_promise_fulfill(c->result);
    return;
  }
  settle_like(c->result, source);
}

static void element_run(TzReactor *r, TzPromise *source) {
  Element *e = (Element *)((char *)r - offsetof(Element, reactor));
  Combine *c = e->all;
  if (c->result->state != TZ_PROMISE_PENDING) {
    return;
  }
  int fulfilled = source->state == TZ_PROMISE_FULFILLED;
  const TzType *type;
  char *slot;
  switch (c->kind) {
  case TZ_COMBINE_RACE:
    settle_from(c, e->index, source);
    return;
  case TZ_COMBINE_ALL:
    if (!fulfilled) {
      retain(source->error);
      tz_promise_reject(c->result, source->error);
      return;
    }
    if (!c->container) {
      break; // Promise<void>: nothing to collect
    }
    slot = slot_of(c, e->index, &type);
    memcpy(slot, value_of(source), type->size);
    tz_value_retain(type, slot);
    break;
  case TZ_COMBINE_ALL_SETTLED:
    slot = slot_of(c, e->index, &type);
    *(void **)slot = settled_result(c->shape->each ? &c->shape->each[e->index] : c->shape, source);
    break;
  case TZ_COMBINE_ANY:
    if (fulfilled) {
      settle_from(c, e->index, source);
      return;
    }
    break;
  }
  if (--c->remaining == 0) {
    combine_done(c);
  }
}

TzPromise *tz_promise_combine(int32_t kind, TzArray *promises, const TzCombine *shape) {
  TZ_ORDERED();
  TzPromise *result = tz_promise_new(shape->result);
  Combine *c = (Combine *)tz_obj_alloc(sizeof(Combine));
  c->base.rc = 1;
  c->base.desc = &combine_desc;
  c->result = result;
  retain(result);
  c->remaining = 1;
  c->kind = kind;
  c->shape = shape;
  c->container = NULL;
  uint32_t count = promises->length;
  if ((kind == TZ_COMBINE_ALL || kind == TZ_COMBINE_ALL_SETTLED) &&
      (shape->tuple || shape->items)) {
    if (shape->tuple) {
      char *tuple = (char *)tz_obj_alloc(shape->tuple_size);
      memset(tuple, 0, shape->tuple_size);
      ((TzInstance *)tuple)->rc = 1;
      ((TzInstance *)tuple)->layout = shape->tuple;
      c->container = tuple;
    } else {
      c->container = tz_arr_new(shape->items, count, count);
    }
  }
  for (uint32_t i = 0; i < count; ++i) {
    TzPromise *p = ((TzPromise **)promises->data)[i];
    Element *e = (Element *)tz_obj_alloc(sizeof(Element));
    e->base.rc = 1;
    e->base.desc = &element_desc;
    e->reactor.run = element_run;
    e->reactor.owner = &e->base;
    e->reactor.source = NULL;
    e->reactor.ahead = 0;
    e->all = c;
    retain(c);
    e->index = i;
    ++c->remaining;
    tz_promise_react(p, &e->reactor);
  }
  // Promise.race of nothing never settles; the others are done at once.
  if (--c->remaining == 0 && kind != TZ_COMBINE_RACE) {
    combine_done(c);
  }
  release(c);

  return result;
}

//===----------------------------------------------------------------------===//
// queueMicrotask and process.nextTick
//===----------------------------------------------------------------------===//

typedef struct {
  TzObject base;
  TzReactor reactor;
  TzFunction callback;
} Task;

static void task_drop(void *object) {
  Task *t = (Task *)object;
  tz_obj_release(t->callback.env);
  tz_obj_free(t);
}

static const TzObjectDesc task_desc = {task_drop};

static void task_run(TzReactor *r, TzPromise *source) {
  (void)source;
  call(((Task *)((char *)r - offsetof(Task, reactor)))->callback);
}

void tz_queue_microtask(const TzFunctionInfo *info, TzObject *env) {
  TZ_ORDERED();
  Task *t = (Task *)tz_obj_alloc(sizeof(Task));
  t->base.rc = 1;
  t->base.desc = &task_desc;
  t->reactor.run = task_run;
  t->reactor.owner = &t->base;
  t->reactor.source = NULL;
  t->reactor.ahead = 0;
  t->callback.info = info;
  t->callback.env = env;
  tz_microtask(&t->reactor);
}

typedef struct Tick {
  struct Tick *next;
  TzFunction callback;
} Tick;

Tick *tz_tick_head, *tz_tick_tail;

void tz_next_tick(const TzFunctionInfo *info, TzObject *env) {
  TZ_ORDERED();
  Tick *t = (Tick *)tz_alloc(sizeof(Tick));
  t->next = NULL;
  t->callback.info = info;
  t->callback.env = env;
  if (tz_tick_tail) {
    tz_tick_tail->next = t;
  } else {
    tz_tick_head = t;
  }
  tz_tick_tail = t;
}

// The rejections that nothing handled by now end the program, as an
// uncaught exception: the first of them is reported.
static int process_rejections(void) {
  size_t count = tz_rejection_count;
  for (size_t i = 0; i < count; ++i) {
    TzPromise *p = tz_rejections[i];
    if (!(p->flags & HANDLED)) {
      retain(p->error);
      tz_thrown = p->error;
      tz_uncaught();
    }
  }
  for (size_t i = 0; i < count; ++i) {
    tz_rejections[i]->flags &= ~LISTED;
    release(tz_rejections[i]);
  }
  tz_rejection_count = 0;

  return 0;
}

// After a callback (processTicksAndRejections in Node): the nextTick
// callbacks, then the microtasks, while either has something; then the
// rejections.
void tz_after_callback(void) {
  do {
    while (tz_tick_head) {
      Tick *t = tz_tick_head;
      tz_tick_head = t->next;
      if (!tz_tick_head) {
        tz_tick_tail = NULL;
      }
      TzFunction callback = t->callback;
      tz_free(t);
      call(callback);
      tz_obj_release(callback.env);
    }
    tz_run_microtasks();
  } while (tz_tick_head || (tz_rejection_count && process_rejections()));
}

//===----------------------------------------------------------------------===//
// Timers and immediates
//===----------------------------------------------------------------------===//

// The timers of one duration, in the order they were set (a TimersList).
typedef struct TimerList {
  int64_t expiry;
  int64_t id; // the order of lists that expire together
  int64_t msecs;
  TzTimer *head, *tail;
  uint32_t position; // in the heap
  int processing;    // listOnTimeout is in it: it frees the list
  int removed;       // gone from the heap and the map meanwhile
} TimerList;

#define TIMEOUT_MAX 2147483647.0

TimerList **tz_timer_heap; // ordered by expiry, then id
size_t tz_timer_heap_size, tz_timer_heap_capacity;
TimerList **tz_timer_lists; // by duration (timerListMap)
size_t tz_timer_list_count, tz_timer_list_capacity;
int64_t tz_timer_list_id;
int64_t tz_timer_refs; // timers set and not unref()'d
TzTimer *tz_immediate_head, *tz_immediate_tail;
int64_t tz_immediate_refs;
const TzFrame *tz_main_await; // where top-level code waits

static int64_t loop_now(void) { return (int64_t)floor(tz_performance_now()); }

static int list_before(const TimerList *a, const TimerList *b) {
  return a->expiry != b->expiry ? a->expiry < b->expiry : a->id < b->id;
}

static void heap_set(size_t i, TimerList *list) {
  tz_timer_heap[i] = list;
  list->position = (uint32_t)i;
}

static void heap_up(size_t i) {
  TimerList *list = tz_timer_heap[i];
  while (i > 0 && list_before(list, tz_timer_heap[(i - 1) / 2])) {
    heap_set(i, tz_timer_heap[(i - 1) / 2]);
    i = (i - 1) / 2;
  }
  heap_set(i, list);
}

static void heap_down(size_t i) {
  TimerList *list = tz_timer_heap[i];
  size_t n = tz_timer_heap_size;
  for (;;) {
    size_t child = 2 * i + 1;
    if (child >= n) {
      break;
    }
    if (child + 1 < n && list_before(tz_timer_heap[child + 1], tz_timer_heap[child])) {
      ++child;
    }
    if (!list_before(tz_timer_heap[child], list)) {
      break;
    }
    heap_set(i, tz_timer_heap[child]);
    i = child;
  }
  heap_set(i, list);
}

static void heap_insert(TimerList *list) {
  if (tz_timer_heap_size == tz_timer_heap_capacity) {
    size_t capacity = tz_timer_heap_capacity ? tz_timer_heap_capacity * 2 : 8;
    TimerList **grown = (TimerList **)tz_alloc(capacity * sizeof *grown);
    if (tz_timer_heap_size) {
      memcpy(grown, tz_timer_heap, tz_timer_heap_size * sizeof *grown);
    }
    tz_free(tz_timer_heap);
    tz_timer_heap = grown;
    tz_timer_heap_capacity = capacity;
  }
  heap_set(tz_timer_heap_size++, list);
  heap_up(tz_timer_heap_size - 1);
}

static void heap_remove(TimerList *list) {
  size_t i = list->position;
  TimerList *last = tz_timer_heap[--tz_timer_heap_size];
  if (i == tz_timer_heap_size) {
    return;
  }
  heap_set(i, last);
  heap_up(i);
  heap_down(last->position);
}

static TimerList *find_list(int64_t msecs) {
  for (size_t i = 0; i < tz_timer_list_count; ++i) {
    if (tz_timer_lists[i]->msecs == msecs) {
      return tz_timer_lists[i];
    }
  }

  return NULL;
}

// Out of the heap and the map; freed unless listOnTimeout is in it.
static void drop_list(TimerList *list) {
  heap_remove(list);
  for (size_t i = 0; i < tz_timer_list_count; ++i) {
    if (tz_timer_lists[i] == list) {
      tz_timer_lists[i] = tz_timer_lists[--tz_timer_list_count];
      break;
    }
  }
  if (list->processing) {
    list->removed = 1;
  } else {
    tz_free(list);
  }
}

static void timer_drop(void *object) {
  TzTimer *t = (TzTimer *)object;
  tz_obj_release(t->callback.env);
  tz_obj_free(t);
}

const TzLayout tz_timeout_layout = {timer_drop, NULL, 0, TZ_LAYOUT_TIMER, "Timeout", 7, 0};
const TzLayout tz_immediate_layout = {timer_drop, NULL, 0, TZ_LAYOUT_TIMER, "Immediate", 9, 0};

// insert() of Node: the timer goes to the end of the list of its duration,
// which the list holds it in.
static void insert(TzTimer *t, double after, int64_t start) {
  int64_t msecs = (int64_t)after; // truncated, as Node does
  t->idle_start = start;
  TimerList *list = find_list(msecs);
  if (!list) {
    list = (TimerList *)tz_alloc(sizeof(TimerList));
    memset(list, 0, sizeof *list);
    list->expiry = start + msecs;
    list->id = tz_timer_list_id++;
    list->msecs = msecs;
    if (tz_timer_list_count == tz_timer_list_capacity) {
      size_t capacity = tz_timer_list_capacity ? tz_timer_list_capacity * 2 : 8;
      TimerList **grown = (TimerList **)tz_alloc(capacity * sizeof *grown);
      if (tz_timer_list_count) {
        memcpy(grown, tz_timer_lists, tz_timer_list_count * sizeof *grown);
      }
      tz_free(tz_timer_lists);
      tz_timer_lists = grown;
      tz_timer_list_capacity = capacity;
    }
    tz_timer_lists[tz_timer_list_count++] = list;
    heap_insert(list);
  }
  t->list = list;
  t->next = NULL;
  t->prev = list->tail;
  if (list->tail) {
    list->tail->next = t;
  } else {
    list->head = t;
  }
  list->tail = t;
  retain(t);
}

// L.remove: out of its list, which held it.
static void unlink_timer(TzTimer *t) {
  TimerList *list = t->list;
  if (!list) {
    return;
  }
  if (t->prev) {
    t->prev->next = t->next;
  } else {
    list->head = t->next;
  }
  if (t->next) {
    t->next->prev = t->prev;
  } else {
    list->tail = t->prev;
  }
  t->prev = t->next = NULL;
  t->list = NULL;
  release(t);
}

TzTimer *tz_timer_new(const TzFunctionInfo *info, TzObject *env, double after, int32_t repeat) {
  TZ_ORDERED();
  if (!(after >= 1 && after <= TIMEOUT_MAX)) {
    after = 1; // as Node and browsers do, also for NaN
  }
  TzTimer *t = (TzTimer *)tz_obj_alloc(sizeof(TzTimer));
  memset(t, 0, sizeof *t);
  t->rc = 1;
  t->layout = &tz_timeout_layout;
  t->callback.info = info;
  t->callback.env = env;
  t->idle_timeout = after;
  t->repeat = repeat ? after : 0;
  t->refed = 1;
  ++tz_timer_refs;
  insert(t, after, loop_now());

  return t;
}

// clearTimeout() and clearInterval(): unenroll() of Node.
void tz_timer_clear(TzTimer *t) {
  TZ_ORDERED();
  if (!t || t->immediate || t->cleared) {
    return;
  }
  t->cleared = 1; // `_onTimeout = null`
  if (t->destroyed) {
    return;
  }
  t->destroyed = 1;
  TimerList *list = t->list;
  retain(t);
  unlink_timer(t);
  if (t->refed) {
    // Only a list emptied by a timer that keeps the loop alive goes.
    if (list && !list->head && !list->removed && find_list(list->msecs) == list) {
      drop_list(list);
    }
    --tz_timer_refs;
  }
  t->idle_timeout = -1;
  release(t);
}

void tz_timer_ref(TzTimer *t, int32_t ref) {
  TZ_ORDERED();
  if (t->immediate) {
    if (ref && !t->refed) {
      t->refed = 1;
      ++tz_immediate_refs;
    } else if (!ref && t->refed) {
      t->refed = 0;
      --tz_immediate_refs;
    }
    return;
  }
  if (ref && !t->refed) {
    t->refed = 1;
    if (!t->destroyed) {
      ++tz_timer_refs;
    }
  } else if (!ref && t->refed) {
    t->refed = 0;
    if (!t->destroyed) {
      --tz_timer_refs;
    }
  }
}

int32_t tz_timer_has_ref(const TzTimer *t) { return t->refed; }

// refresh(): the same timer again from now, at the end of its list.
void tz_timer_refresh(TzTimer *t) {
  TZ_ORDERED();
  if (t->immediate || t->idle_timeout < 0) {
    return;
  }
  retain(t);
  unlink_timer(t);
  insert(t, t->idle_timeout, loop_now());
  if (t->destroyed) {
    t->destroyed = 0;
    if (t->refed) {
      ++tz_timer_refs;
    }
  }
  release(t);
}

TzTimer *tz_immediate_new(const TzFunctionInfo *info, TzObject *env) {
  TZ_ORDERED();
  TzTimer *t = (TzTimer *)tz_obj_alloc(sizeof(TzTimer));
  memset(t, 0, sizeof *t);
  t->rc = 2; // the program's and the queue's
  t->layout = &tz_immediate_layout;
  t->callback.info = info;
  t->callback.env = env;
  t->immediate = 1;
  t->refed = 1;
  ++tz_immediate_refs;
  t->prev = tz_immediate_tail;
  if (tz_immediate_tail) {
    tz_immediate_tail->next = t;
  } else {
    tz_immediate_head = t;
  }
  tz_immediate_tail = t;

  return t;
}

void tz_immediate_clear(TzTimer *t) {
  TZ_ORDERED();
  if (!t || !t->immediate || t->destroyed) {
    return;
  }
  t->destroyed = 1;
  if (t->refed) {
    --tz_immediate_refs;
  }
  t->refed = 0;
  // Out of the queue, or of the batch being run.
  if (t->next) {
    t->next->prev = t->prev;
  }
  if (t->prev) {
    t->prev->next = t->next;
  }
  if (t == tz_immediate_head) {
    tz_immediate_head = t->next;
  }
  if (t == tz_immediate_tail) {
    tz_immediate_tail = t->prev;
  }
  t->next = t->prev = NULL;
  release(t);
}

// listOnTimeout of Node.
static void list_on_timeout(TimerList *list, int64_t now) {
  int64_t msecs = list->msecs;
  int ran = 0;
  list->processing = 1;
  TzTimer *t;
  while ((t = list->head) != NULL) {
    if (now - t->idle_start < msecs) {
      // The rest of the list comes later.
      list->expiry = t->idle_start + msecs > now + 1 ? t->idle_start + msecs : now + 1;
      list->id = tz_timer_list_id++;
      if (!list->removed) {
        heap_down(list->position);
      }
      list->processing = 0;
      if (list->removed) {
        tz_free(list);
      }
      return;
    }
    if (ran) {
      tz_after_callback();
    }
    ran = 1;
    if (list->head != t) {
      continue; // the nextTick callbacks cleared it
    }
    retain(t);
    unlink_timer(t);
    int64_t start = t->repeat ? loop_now() : 0;
    call(t->callback);
    if (t->repeat && t->idle_timeout != -1) {
      t->idle_timeout = t->repeat;
      insert(t, t->idle_timeout, start);
    } else if (!t->list && !t->destroyed) {
      t->destroyed = 1;
      if (t->refed) {
        --tz_timer_refs;
      }
    }
    release(t);
  }
  list->processing = 0;
  if (!list->removed && find_list(msecs) == list) {
    drop_list(list);
  } else if (list->removed) {
    tz_free(list);
  }
}

// processTimers of Node: the lists that expired, by when, each followed by
// the nextTick callbacks and the microtasks.
static void run_timers(void) {
  int64_t now = loop_now();
  int ran = 0;
  while (tz_timer_heap_size) {
    TimerList *list = tz_timer_heap[0];
    if (list->expiry > now) {
      break;
    }
    if (ran) {
      tz_after_callback();
    }
    ran = 1;
    list_on_timeout(list, now);
  }
  if (ran) {
    tz_after_callback();
  }
}

// processImmediate of Node: those set before it started, each followed by
// the nextTick callbacks and the microtasks; those they set wait for the
// next turn of the loop. The one that ran last is held until the next is
// found: clearImmediate() unlinks the others from the batch.
static void run_immediates(void) {
  TzTimer *first = tz_immediate_head, *prev = NULL;
  if (!first) {
    return;
  }
  tz_immediate_head = tz_immediate_tail = NULL;
  for (;;) {
    TzTimer *t = prev ? prev->next : first;
    if (!t) {
      break;
    }
    t->destroyed = 1;
    if (t->refed) {
      --tz_immediate_refs;
    }
    t->refed = 0;
    TzFunction callback = t->callback;
    t->callback.env = NULL;
    call(callback);
    tz_obj_release(callback.env);
    if (prev) {
      release(prev);
    }
    prev = t;
    tz_after_callback();
  }
  release(prev);
}

static int loop_alive(void) { return tz_timer_refs > 0 || tz_immediate_refs > 0 || tz_io_alive(); }

// The loop of libuv, once the top-level code ran: it ends when no timer,
// immediate or work of the pool keeps it alive. The top-level code of an ES
// module runs inside a microtask, so the microtasks come before the nextTick
// callbacks then; that of CommonJS is a callback like any other. The poll
// phase waits for the pool (io.c) until the next timer is due; with an
// immediate to run, it does not wait.
void tz_async_run(int32_t es_module) {
  if (es_module) {
    tz_run_microtasks();
  }
  tz_after_callback();
  if (loop_alive()) {
    run_timers();
  }
  while (loop_alive()) {
    int64_t wait = -1;
    if (tz_immediate_head) {
      wait = 0;
    } else if (tz_timer_heap_size) {
      wait = tz_timer_heap[0]->expiry - loop_now();
      if (wait < 0) {
        wait = 0;
      }
    }
    tz_io_poll(wait);
    run_immediates();
    run_timers();
  }
}

//===----------------------------------------------------------------------===//
// node:timers/promises
//===----------------------------------------------------------------------===//

// setTimeout(delay, value, { signal, ref }) and setImmediate(value, ...) of
// node:timers/promises: a timer of Node's (the same lists as setTimeout)
// that fulfills the promise with the value it holds. With a signal, the
// promise the program has follows the one the timer settles through
// finally(), as in lib/timers/promises.js, which takes the same turns of the
// queue; an abort clears the timer and rejects with an AbortError.
typedef struct Wait {
  TzObject base;
  TzReactor reactor; // the finally() of Node, with a signal
  TzAbortListener listener;
  TzPromise *promise; // what the program has
  TzPromise *inner;   // with a signal: what the timer settles
  TzTimer *timer;     // not held: the list or the queue holds it until done
  TzSignal *signal;
  int32_t immediate, done;
  // While the finally() waits: the reaction holds the Wait, which holds the
  // promise it waits for. If the loop ends first (the timer was unref()'d),
  // shutdown breaks that.
  struct Wait *prev, *next;
} Wait;

struct Wait *tz_waits;

static void unlist_wait(Wait *w) {
  if (w->prev) {
    w->prev->next = w->next;
  } else if (tz_waits == w) {
    tz_waits = w->next;
  }
  if (w->next) {
    w->next->prev = w->prev;
  }
  w->prev = w->next = NULL;
}

// The value a pending promise holds, which nothing else would release.
static void drop_pending_value(TzPromise *p) {
  const TzType *type = value_type(p);
  if (p->state != TZ_PROMISE_PENDING || !type) {
    return;
  }
  tz_value_release(type, value_of(p));
  memset(value_of(p), 0, type->size);
}

static void wait_drop(void *object) {
  // It leaves lists of the runtime: in node's order (parallel.c).
  TZ_ORDERED();
  Wait *w = (Wait *)object;
  unlist_wait(w);
  tz_signal_unlisten(&w->listener);
  if (w->inner) {
    drop_pending_value(w->inner);
    release(w->inner);
  }
  drop_pending_value(w->promise);
  release(w->promise);
  if (w->signal) {
    release(w->signal);
  }
  tz_obj_free(w);
}

static const TzObjectDesc wait_desc = {wait_drop};

static void wait_fired(TzObject *env) {
  Wait *w = (Wait *)env;
  if (w->done) {
    return;
  }
  w->done = 1;
  tz_promise_fulfill(w->inner ? w->inner : w->promise);
}

static const TzFunctionInfo wait_info = {(void *)wait_fired, {NULL, 0}};

static void wait_aborted(TzAbortListener *l) {
  Wait *w = (Wait *)((char *)l - offsetof(Wait, listener));
  if (w->done) {
    return;
  }
  w->done = 1;
  ++w->base.rc;
  if (w->immediate) {
    tz_immediate_clear(w->timer);
  } else {
    tz_timer_clear(w->timer);
  }
  drop_pending_value(w->inner);
  tz_promise_reject(w->inner, tz_abort_error(w->signal));
  tz_obj_release(&w->base);
}

// The finally() callback: the listener goes, and the promise of the program
// settles as Node's does after it.
static void wait_finally(TzReactor *r, TzPromise *source) {
  Wait *w = (Wait *)((char *)r - offsetof(Wait, reactor));
  unlist_wait(w);
  tz_signal_unlisten(&w->listener);
  tz_promise_finally(w->promise, source, NULL);
}

void tz_timers_wait(TzPromise *p, double delay, int32_t immediate, TzSignal *signal, int32_t ref) {
  TZ_ORDERED();
  if (signal && signal->aborted) {
    drop_pending_value(p);
    tz_promise_reject(p, tz_abort_error(signal));
    return;
  }
  Wait *w = (Wait *)tz_obj_alloc(sizeof(Wait));
  memset(w, 0, sizeof *w);
  w->base.rc = 1;
  w->base.desc = &wait_desc;
  w->promise = p;
  retain(p);
  w->immediate = immediate;
  if (signal) {
    // The value moves to the promise the timer settles.
    w->inner = tz_promise_new(p->layout);
    const TzType *type = value_type(p);
    if (type) {
      memcpy(value_of(w->inner), value_of(p), type->size);
      memset(value_of(p), 0, type->size);
    }
    w->signal = signal;
    retain(signal);
    w->listener.run = wait_aborted;
    tz_signal_listen(signal, &w->listener);
    w->reactor.run = wait_finally;
    w->reactor.owner = &w->base;
    w->reactor.ahead = 0;
    ++w->base.rc; // the reaction's
    tz_promise_react(w->inner, &w->reactor);
    w->next = tz_waits;
    if (tz_waits) {
      tz_waits->prev = w;
    }
    tz_waits = w;
  }
  ++w->base.rc; // the timer's
  TzTimer *timer = immediate ? tz_immediate_new(&wait_info, &w->base)
                             : tz_timer_new(&wait_info, &w->base, delay, 0);
  w->timer = timer;
  if (!ref) {
    tz_timer_ref(timer, 0);
  }
  release(timer);
  release(w);
}

// At the end: the waits whose timers never fired let their reactions go.
static void drop_waits(void) {
  while (tz_waits) {
    Wait *w = tz_waits;
    unlist_wait(w);
    w->inner->head = w->inner->tail = NULL;
    tz_obj_release(&w->base);
  }
}

//===----------------------------------------------------------------------===//
// Async iterators and async generators (docs/iterators.md, step 4)
//===----------------------------------------------------------------------===//

void tz_async_iter_next(void *it, TzPromise *p) {
  TZ_ORDERED();
  ((const TzAsyncIterLayout *)((TzInstance *)it)->layout)->next(it, p);
}

void tz_async_iter_close(void *it, TzPromise *p) {
  TZ_ORDERED();
  ((const TzAsyncIterLayout *)((TzInstance *)it)->layout)->close(it, p);
}

// The fulfilled promise of a step that gives no value: the iterator is done.
static void fulfill_done(TzPromise *p) {
  p->flags |= TZ_PROMISE_DONE;
  tz_promise_fulfill(p);
}

// Fulfills `p`, whose value is there already, `turns` turns of the queue
// later: the turns the code of Node takes in between.
typedef struct {
  TzObject base;
  TzReactor reactor;
  TzPromise *promise;
  int32_t turns;
} Turns;

static void turns_drop(void *object) {
  release(((Turns *)object)->promise);
  tz_obj_free(object);
}

static const TzObjectDesc turns_desc = {turns_drop};

static void turns_run(TzReactor *r, TzPromise *source) {
  (void)source;
  Turns *t = (Turns *)((char *)r - offsetof(Turns, reactor));
  if (--t->turns > 0) {
    ++t->base.rc;
    tz_microtask(r);
    return;
  }
  tz_promise_fulfill(t->promise);
}

static void fulfill_after(TzPromise *p, int32_t turns) {
  if (turns <= 0) {
    tz_promise_fulfill(p);
    return;
  }
  Turns *t = (Turns *)tz_obj_alloc(sizeof(Turns));
  t->base.rc = 1;
  t->base.desc = &turns_desc;
  t->reactor.next = NULL;
  t->reactor.run = turns_run;
  t->reactor.owner = &t->base;
  t->reactor.source = NULL;
  t->reactor.ahead = 0;
  t->promise = p;
  retain(p);
  t->turns = turns;
  tz_microtask(&t->reactor);
}

// An async generator: the queue of the steps asked of it (next() and
// return()) and the state of the spec (AsyncGeneratorState), then the
// reactor its coroutine awaits with, and then its frame
// (compiler/lower/codegen/codegen_async_iter.cpp). A step is served by running
// the body to its next `yield` or its end; a next() asked while it runs waits
// in the queue, as AsyncGeneratorEnqueue says.
enum { AGEN_START, AGEN_YIELD, AGEN_RUNNING, AGEN_RETURNING, AGEN_DONE };
_Static_assert(offsetof(TzAsyncGen, reactor) == 40 && sizeof(TzAsyncGen) == 80,
               "TzAsyncGen must match kAgenReactor and kAgenHeader of CodeGen");

static void agen_enqueue(TzAsyncGen *g, TzPromise *p, int32_t close) {
  TzAgenStep *s = (TzAgenStep *)tz_alloc(sizeof(TzAgenStep));
  s->next = NULL;
  s->promise = p;
  retain(p);
  s->close = close;
  if (g->tail) {
    g->tail->next = s;
  } else {
    g->head = s;
  }
  g->tail = s;
}

static TzAgenStep *agen_pop(TzAsyncGen *g) {
  TzAgenStep *s = g->head;
  if (s) {
    g->head = s->next;
    if (!g->head) {
      g->tail = NULL;
    }
  }

  return s;
}

// AsyncGeneratorResume: the body goes on, here, until it waits.
static void agen_resume(TzAsyncGen *g) {
  g->state = AGEN_RUNNING;
  retain(g);
  g->reactor.run(&g->reactor, NULL);
  release(g);
}

static void agen_drain(TzAsyncGen *g);

// AsyncGeneratorAwaitReturn of return() when the body does not run: a turn
// of the queue, then done.
typedef struct {
  TzObject base;
  TzReactor reactor;
  TzAsyncGen *generator;
} AgenReturn;

static void agen_return_drop(void *object) {
  release(((AgenReturn *)object)->generator);
  tz_obj_free(object);
}

static const TzObjectDesc agen_return_desc = {agen_return_drop};

static void agen_return_run(TzReactor *r, TzPromise *source) {
  (void)source;
  TzAsyncGen *g = ((AgenReturn *)((char *)r - offsetof(AgenReturn, reactor)))->generator;
  g->state = AGEN_DONE;
  TzAgenStep *s = agen_pop(g);
  if (s) {
    fulfill_done(s->promise);
    release(s->promise);
    tz_free(s);
  }
  agen_drain(g);
}

static void agen_await_return(TzAsyncGen *g) {
  g->state = AGEN_RETURNING;
  AgenReturn *job = (AgenReturn *)tz_obj_alloc(sizeof(AgenReturn));
  job->base.rc = 1;
  job->base.desc = &agen_return_desc;
  job->reactor.next = NULL;
  job->reactor.run = agen_return_run;
  job->reactor.owner = &job->base;
  job->reactor.source = NULL;
  job->reactor.ahead = 0;
  job->generator = g;
  retain(g);
  tz_microtask(&job->reactor);
}

// AsyncGeneratorDrainQueue: once done, each next() waiting is done too, up
// to a return(), which takes its turn first.
static void agen_drain(TzAsyncGen *g) {
  while (g->head) {
    if (g->head->close) {
      agen_await_return(g);
      return;
    }
    TzAgenStep *s = agen_pop(g);
    fulfill_done(s->promise);
    release(s->promise);
    tz_free(s);
  }
}

// next() of an async generator, through its layout.
void tz_agen_next(void *it, TzPromise *p) {
  TZ_ORDERED();
  TzAsyncGen *g = (TzAsyncGen *)it;
  if (g->state == AGEN_DONE) {
    fulfill_done(p);
    return;
  }
  agen_enqueue(g, p, 0);
  if (g->state == AGEN_START || g->state == AGEN_YIELD) {
    agen_resume(g);
  }
}

// return() of an async generator: `for await` left early.
void tz_agen_close(void *it, TzPromise *p) {
  TZ_ORDERED();
  TzAsyncGen *g = (TzAsyncGen *)it;
  agen_enqueue(g, p, 1);
  if (g->state == AGEN_START || g->state == AGEN_DONE) {
    agen_await_return(g);
  } else if (g->state == AGEN_YIELD) {
    agen_resume(g);
  }
}

// `yield` of the body, after it awaited the value: AsyncGeneratorYield. The
// value at `value` (of `type`, owned) settles the first step; then the body
// goes on at once for the next step waiting (0 for next(), 1 for return()),
// or waits for one (-1).
int32_t tz_agen_yield(TzAsyncGen *g, void *value, const TzType *type) {
  TZ_ORDERED();
  TzAgenStep *s = agen_pop(g);
  if (s) {
    const TzType *held = value_type(s->promise);
    if (held) {
      memcpy(value_of(s->promise), value, held->size);
    } else if (type && type->size) {
      tz_value_release(type, value); // a return() whose finally yields
    }
    tz_promise_fulfill(s->promise);
    release(s->promise);
    tz_free(s);
  } else if (type && type->size) {
    tz_value_release(type, value);
  }
  if (g->head) {
    return g->head->close;
  }
  g->state = AGEN_YIELD;

  return -1;
}

// After the body was resumed at a `yield`: by return() (1) or next() (0).
int32_t tz_agen_resumed(TzAsyncGen *g) { return g->head ? g->head->close : 0; }

// The body is done, by its end, `return` or an error (owned), which rejects
// the step it ran for.
void tz_agen_finish(TzAsyncGen *g, void *error) {
  TZ_ORDERED();
  g->state = AGEN_DONE;
  TzAgenStep *s = agen_pop(g);
  if (s) {
    if (error) {
      tz_promise_reject(s->promise, error);
    } else {
      fulfill_done(s->promise);
    }
    release(s->promise);
    tz_free(s);
  } else if (error) {
    release(error);
  }
  agen_drain(g);
}

// Before the generator is freed: the steps that still wait.
void tz_agen_drop_steps(TzAsyncGen *g) {
  while (g->head) {
    TzAgenStep *s = agen_pop(g);
    release(s->promise);
    tz_free(s);
  }
}

//===----------------------------------------------------------------------===//
// setInterval of node:timers/promises
//===----------------------------------------------------------------------===//

// An async generator of Node's (lib/timers/promises.js): an interval that
// counts the times it fired, and a step of `for await` that takes one each,
// or waits for the next. As the body of the generator, the interval starts
// with the first step. An abort ends it with an AbortError.
typedef struct {
  TzInstance base; // an async iterator
  TzTimer *timer;  // held, once started
  TzSignal *signal;
  TzAbortListener listener;
  TzPromise *waiting; // a step waiting for the interval
  const TzType *type; // of the value, NULL for none
  double delay;
  int64_t fired;   // not given yet
  int32_t done;    // closed, or aborted
  int32_t aborted; // the next step is rejected with an AbortError
  int32_t started, ref;
  uint8_t value[]; // of `type`
} Interval;

static void interval_drop(void *object);
static void interval_next(void *it, TzPromise *p);
static void interval_close(void *it, TzPromise *p);

const TzAsyncIterLayout tz_interval_layout = {
    {interval_drop, NULL, 0, TZ_LAYOUT_ASYNC_ITERATOR | TZ_LAYOUT_ASYNC_GENERATOR, NULL, 0, 0},
    interval_next,
    interval_close};

// The value, copied into the step `p`, `turns` turns of the queue later.
static void interval_give(Interval *it, TzPromise *p, int32_t turns) {
  const TzType *held = value_type(p);
  if (held) {
    memcpy(value_of(p), it->value, held->size);
    tz_value_retain(held, value_of(p));
  }
  fulfill_after(p, turns);
}

// The interval stops; a step waiting gets `reject` (an AbortError) or done.
static void interval_stop(Interval *it, void *error) {
  it->done = 1;
  tz_signal_unlisten(&it->listener);
  if (it->timer) {
    tz_timer_clear(it->timer);
    release(it->timer);
    it->timer = NULL;
  }
  TzPromise *p = it->waiting;
  it->waiting = NULL;
  if (p) {
    if (error) {
      tz_promise_reject(p, error);
    } else {
      fulfill_done(p);
    }
    release(p);
  } else if (error) {
    release(error);
  }
}

static void interval_drop(void *object) {
  // It leaves lists of the runtime: in node's order (parallel.c).
  TZ_ORDERED();
  Interval *it = (Interval *)object;
  interval_stop(it, NULL);
  if (it->signal) {
    release(it->signal);
  }
  if (it->type) {
    tz_value_release(it->type, it->value);
  }
  tz_obj_free(it);
}

// The interval fired: a step waiting takes it, else it is counted. The
// generator of Node goes on a turn after the interval resolves what it
// awaits, and its `yield` awaits the value: two turns.
typedef struct {
  TzObject base;
  Interval *interval; // not held: the timer holds this, and the iterator the timer
} IntervalTick;

static void interval_tick_drop(void *object) { tz_obj_free(object); }

static const TzObjectDesc interval_tick_desc = {interval_tick_drop};

static void interval_fired(TzObject *env) {
  Interval *it = ((IntervalTick *)env)->interval;
  if (it->done) {
    return;
  }
  TzPromise *p = it->waiting;
  if (!p) {
    ++it->fired;
    return;
  }
  it->waiting = NULL;
  interval_give(it, p, 2);
  release(p);
}

static const TzFunctionInfo interval_info = {(void *)interval_fired, {NULL, 0}};

static void interval_aborted(TzAbortListener *l) {
  Interval *it = (Interval *)((char *)l - offsetof(Interval, listener));
  if (it->done) {
    return;
  }
  // A step waiting is rejected now; else the next one is.
  it->aborted = it->waiting == NULL;
  interval_stop(it, tz_abort_error(it->signal));
}

static void interval_start(Interval *it) {
  it->started = 1;
  if (it->signal && it->signal->aborted) {
    it->aborted = 1;
    it->done = 1;
    return;
  }
  IntervalTick *tick = (IntervalTick *)tz_obj_alloc(sizeof(IntervalTick));
  tick->base.rc = 1;
  tick->base.desc = &interval_tick_desc;
  tick->interval = it;
  it->timer = tz_timer_new(&interval_info, &tick->base, it->delay, 1);
  if (!it->ref) {
    tz_timer_ref(it->timer, 0);
  }
  if (it->signal) {
    it->listener.run = interval_aborted;
    tz_signal_listen(it->signal, &it->listener);
  }
}

static void interval_next(void *object, TzPromise *p) {
  Interval *it = (Interval *)object;
  if (!it->started) {
    interval_start(it);
  }
  if (it->aborted) {
    // The generator of Node throws once, then it is done.
    it->aborted = 0;
    tz_promise_reject(p, tz_abort_error(it->signal));
    return;
  }
  if (it->done) {
    fulfill_done(p);
    return;
  }
  if (it->fired > 0) {
    // The `yield` of the generator awaits the value: a turn.
    --it->fired;
    interval_give(it, p, 1);
    return;
  }
  // One step at a time: `for await` waits for each.
  if (it->waiting) {
    fulfill_done(p);
    return;
  }
  it->waiting = p;
  retain(p);
}

// return(): the generator of Node awaits at its `yield`, a turn, then
// clears the interval in its `finally`.
static void interval_close(void *object, TzPromise *p) {
  Interval *it = (Interval *)object;
  it->started = 1;
  interval_stop(it, NULL);
  fulfill_after(p, 1);
}

// The value is of `type`, NULL for none.
void *tz_timers_interval(double delay, const void *value, const TzType *type, TzSignal *signal,
                         int32_t ref) {
  size_t size = type ? type->size : 0;
  Interval *it = (Interval *)tz_obj_alloc((int64_t)(sizeof(Interval) + size));
  memset(it, 0, sizeof(Interval));
  it->base.rc = 1;
  it->base.layout = &tz_interval_layout.base;
  it->type = type;
  if (size) {
    memcpy(it->value, value, size);
  }
  it->delay = delay;
  it->ref = ref;
  if (signal) {
    it->signal = signal;
    retain(signal);
  }

  return it;
}

//===----------------------------------------------------------------------===//
// Top-level code that fails
//===----------------------------------------------------------------------===//

// Node reports an error of the top-level code of an ES module a few turns
// of the microtask queue later, when its loader gets to it: four for a
// module without top-level `await`, five with. Jobs queued before it run
// meanwhile. (In CommonJS the program ends at once, as without promises.)
typedef struct {
  TzObject base;
  TzReactor reactor;
  void *error;
  int32_t turns;
} Report;

static void report_drop(void *object) { tz_obj_free(object); }

static const TzObjectDesc report_desc = {report_drop};

static void report_run(TzReactor *r, TzPromise *source) {
  Report *report = (Report *)((char *)r - offsetof(Report, reactor));
  if (source) { // the reaction to the top-level code's promise
    if (source->state != TZ_PROMISE_REJECTED) {
      return;
    }
    retain(source->error);
    report->error = source->error;
  }
  if (--report->turns > 0) {
    ++report->base.rc;
    tz_microtask(r);
    return;
  }
  tz_thrown = report->error;
  tz_uncaught();
}

static Report *new_report(void *error, int32_t turns) {
  Report *report = (Report *)tz_obj_alloc(sizeof(Report));
  report->base.rc = 1;
  report->base.desc = &report_desc;
  report->reactor.run = report_run;
  report->reactor.owner = &report->base;
  report->reactor.source = NULL;
  report->reactor.ahead = 0;
  report->error = error;
  report->turns = turns;

  return report;
}

TZ_NORETURN void tz_async_main_failed(void) {
  void *error = tz_thrown;
  tz_thrown = NULL;
  tz_microtask(&new_report(error, 4)->reactor);
  tz_run_microtasks();
  tz_thrown = error; // not reached: the report ends the program
  tz_uncaught();
}

void tz_async_main(TzPromise *main) {
  tz_promise_react(main, &new_report(NULL, 5)->reactor);
  tz_async_run(1);
  if (main->state != TZ_PROMISE_PENDING) {
    return;
  }
  // Waits for something that nothing will do: as Node, with code 13.
  tz_err_begin();
  static const char text[] = "Warning: Detected unsettled top-level await";
  tz_out_write(text, sizeof text - 1);
  if (tz_main_await) {
    tz_out_write(" at ", 4);
    tz_out_write(tz_main_await->file, tz_main_await->file_length);
    char line[16];
    int n = snprintf(line, sizeof line, ":%u", tz_main_await->line);
    tz_out_write(line, (size_t)n);
  }
  tz_out_write("\n", 1);
  tz_err_end();
  exit(13);
}

// Releases what the runtime holds, before the leak check: the timers and
// immediates that did not keep the loop alive.
void tz_async_shutdown(void) {
  drop_waits();
  while (tz_immediate_head) {
    TzTimer *t = tz_immediate_head;
    tz_immediate_head = t->next;
    release(t);
  }
  tz_immediate_tail = NULL;
  while (tz_timer_heap_size) {
    TimerList *list = tz_timer_heap[0];
    while (list->head) {
      unlink_timer(list->head);
    }
    drop_list(list);
  }
  tz_free(tz_timer_heap);
  tz_free(tz_timer_lists);
  tz_free(tz_rejections);
  tz_timer_heap = NULL;
  tz_timer_lists = NULL;
  tz_rejections = NULL;
  tz_timer_heap_capacity = tz_timer_list_capacity = tz_rejection_capacity = 0;
}
