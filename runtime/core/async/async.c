#include "runtime/core/async/async.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/array/array.h"
#include "runtime/core/closure/closure.h"
#include "runtime/core/error/error.h"
#include "runtime/core/fail/fail.h"
#include "runtime/core/group/group.h"
#include "runtime/core/io/io.h"
#include "runtime/core/output/output.h"
#include "runtime/core/parallel/parallel.h"
#include "runtime/core/publish/publish.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void tz_err_begin(void);
void tz_err_end(void);
double tz_performance_now(void);

enum { LOCKED = 2 };

static void retain(void *object) { tz_ref_retain(object); }

static void release(void *object) { tz_obj_release((TzObject *)object); }

static void call(TzFunction fn) { ((void (*)(TzObject *))fn.info->code)(fn.env); }

TzReactor *tz_micro_head, *tz_micro_tail;

#if TZ_PARALLEL
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
void tz_ahead_dispatch(TzReactor *head);
void tz_ahead_run(TzReactor *r, TzPromise *source);
void tz_ahead_popped(TzReactor *r);
int32_t tz_ahead_keep(int32_t site);
extern int32_t tz_ahead_on;
#endif

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
    TzPromise *source = r->source;
    TzObject *owner = r->owner;
    r->source = NULL;
#if TZ_PARALLEL
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

static const TzType *value_type(const TzPromise *p) {
  return p->layout->count ? p->layout->fields[0].type : NULL;
}

static void *value_of(const TzPromise *p) { return (char *)p + TZ_PROMISE_VALUE; }

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

void tz_promise_drop(void *object) {
  TzPromise *p = (TzPromise *)object;
  const TzType *type = value_type(p);
  int member = p->rc < 0;
  if (p->state == TZ_PROMISE_FULFILLED) {
    if (type && member) {
      tz_value_release_in(p, type, value_of(p));
    } else if (type) {
      tz_value_release(type, value_of(p));
    }
  }
  for (TzReactor *r = p->head; r;) {
    TzReactor *next = r->next;
    if (r->source == TZ_REACTION_WITHIN) {
      r->source = NULL;
    } else if (member && r->owner->rc < 0) {
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
#define HELD_MAX 16
TZ_THREAD_LOCAL TzPromise *tz_held[HELD_MAX];
TZ_THREAD_LOCAL int32_t tz_held_count = -1;

static void trigger(TzPromise *p);

void tz_trigger_hold(void) { tz_held_count = 0; }

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

static void trigger(TzPromise *p) {
#if TZ_PARALLEL
  if (tz_held_count >= 0 && p->head) {
    if (tz_held_count == HELD_MAX) {
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
    p->head = p->tail = NULL;
    for (TzReactor *r = head; r;) {
      TzReactor *next = r->next;
      int within = r->source == TZ_REACTION_WITHIN;
      r->source = p;
      retain(p);
      if (within) {
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
    int within = r->source == TZ_REACTION_WITHIN;
    r->source = p;
    retain(p);
    if (within) {
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
static int reactions_ahead(const TzPromise *p) {
  if (!tz_ahead_early()) {
    return 0;
  }
  for (const TzReactor *r = p->head; r; r = r->next) {
    uintptr_t ahead = r->ahead;
    if (!ahead || ahead >= ((uintptr_t)1 << 20)) {
      return 0;
    }
  }

  return 1;
}
#endif

void tz_promise_fulfill(TzPromise *p) {
#if TZ_PARALLEL
  if (!tz_thread.turn && tz_held_count < 0 &&
      !(tz_ahead_private(p) && (!p->head || reactions_ahead(p)))) {
    tz_order_wait();
  }
#endif
  p->state = TZ_PROMISE_FULFILLED;
  const TzType *type = value_type(p);
#if TZ_PARALLEL
  if (type && !(p->flags & TZ_PROMISE_DONE)) {
    tz_publish_value(type, value_of(p));
  }
#endif
  if (p->rc < 0 && type) {
    tz_group_store_value(p, type, value_of(p));
  }
  trigger(p);
}

void tz_promise_succeed(TzPromise *p, const void *value, size_t size) {
  const TzType *type = value_type(p);
  char *slot = (char *)value_of(p);
  if (type && type->kind == TZ_T_RESULT) {
    const TzResultType *result = (const TzResultType *)type;
    memset(slot, 0, type->size);
    if (size) {
      memcpy(slot + result->value_offset, value, size);
    }
    slot[result->ok_offset] = 1;
  } else if (size) {
    memcpy(slot, value, size);
  }
  tz_promise_fulfill(p);
}

void tz_promise_fail(TzPromise *p, void *error) {
  const TzType *type = value_type(p);
  if (!type || type->kind != TZ_T_RESULT) {
    tz_panicf("internal error: a Promise without a Result failed");
  }
  const TzResultType *result = (const TzResultType *)type;
  char *slot = (char *)value_of(p);
  memset(slot, 0, type->size);
  memcpy(slot + result->error_offset, &error, sizeof error);
  tz_promise_fulfill(p);
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
  if (p->state == TZ_PROMISE_PENDING) {
    r->next = NULL;
    if (p->tail) {
      p->tail->next = r;
    } else {
      p->head = r;
    }
    p->tail = r;
    if (p->rc < 0 && tz_group_store_within(p, r->owner)) {
      r->source = TZ_REACTION_WITHIN;
    }
    return;
  }
  r->source = p;
  retain(p);
  tz_microtask(r);
}

#if TZ_PARALLEL
static void await_unlock(void) {
  TzLock *lock = (TzLock *)tz_thread.await_lock;
  if (!lock) {
    return;
  }
  tz_thread.await_lock = NULL;
  --tz_thread.conn_locks;
  tz_unlock(lock);
}

static int await_ahead(TzReactor *r, TzPromise *p, int32_t ahead) {
  if (!ahead || !tz_ahead_early()) {
    return 0;
  }
  if (!p) {
    r->ahead = (uintptr_t)ahead;
    retain(r->owner);
    if (!tz_ahead_local(r)) {
      tz_microtask(r);
    }
    return 1;
  }
  if (!tz_thread.await_lock && !tz_ahead_private(p)) {
    return 0;
  }
  if (p->state == TZ_PROMISE_PENDING) {
    r->ahead = (uintptr_t)ahead;
    retain(r->owner);
    r->next = NULL;
    if (p->tail) {
      p->tail->next = r;
    } else {
      p->head = r;
    }
    p->tail = r;
    if (p->rc < 0 && tz_group_store_within(p, r->owner)) {
      r->source = TZ_REACTION_WITHIN;
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
  r->source = p;
  retain(p);
  if (!tz_ahead_local(r)) {
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
  TzLock *source = (TzLock *)tz_thread.await_lock;
  if (source && !tz_thread.turn) {
    tz_order_wait();
    tz_lock(source);
    ++tz_thread.conn_locks;
    tz_thread.await_lock = source;
  }
#endif
  TZ_ORDERED();
#if TZ_PARALLEL
  r->ahead = ahead && tz_ahead_on ? (uintptr_t)tz_ahead_keep(ahead) : 0;
#else
  r->ahead = (uintptr_t)ahead;
#endif
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

static void settle_like(TzPromise *target, const TzPromise *source) {
  const TzType *type = value_type(target);
  if (type) {
    memcpy(value_of(target), value_of(source), type->size);
    tz_value_retain(type, value_of(target));
  }
  tz_promise_fulfill(target);
}

typedef struct {
  TzObject base;
  TzReactor reactor;
  TzPromise *target, *origin;
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
  settle_like(f->target, f->first ? f->origin : source);
}

static void follow_then(TzReactor *r, TzPromise *source) {
  (void)source;
  Follow *f = (Follow *)((char *)r - offsetof(Follow, reactor));
  ++f->base.rc;
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

const TzLayout tz_promise_void_layout = {tz_promise_drop, NULL, 0, TZ_LAYOUT_PROMISE, NULL, 0, 0};

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

typedef struct {
  TzObject base;
  TzReactor reactor;
  TzPromise *promise;
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

void tz_promise_fulfill_later(TzPromise *p) { queue_later(p, NULL); }

void tz_promise_react_later(TzPromise *p, TzReactor *r) { queue_later(p, r); }

TzPromise *tz_promise_resolved(const TzLayout *layout) {
  TzPromise *p = tz_promise_new(layout);
  p->state = TZ_PROMISE_FULFILLED;

  return p;
}

typedef struct {
  TzObject base;
  TzPromise *result;
  void *container;
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
    if (tz_rc_is_member(array->rc)) {
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

static void combine_done(Combine *c) {
  if (c->container) {
    *(void **)value_of(c->result) = c->container;
  }
  c->container = NULL;
  tz_promise_fulfill(c->result);
}

static void settle_from(Combine *c, uint32_t i, const TzPromise *source) {
  void (*convert)(void *, const void *) = c->shape->convert ? c->shape->convert[i] : NULL;
  if (convert) {
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
  const TzType *type;
  char *slot;
  switch (c->kind) {
  case TZ_COMBINE_RACE:
    settle_from(c, e->index, source);
    return;
  case TZ_COMBINE_ALL:
    if (!c->container) {
      break;
    }
    slot = slot_of(c, e->index, &type);
    memcpy(slot, value_of(source), type->size);
    tz_value_retain(type, slot);
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
  if (kind == TZ_COMBINE_ALL && (shape->tuple || shape->items)) {
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
  if (--c->remaining == 0 && kind != TZ_COMBINE_RACE) {
    combine_done(c);
  }
  release(c);

  return result;
}

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
  } while (tz_tick_head);
}

typedef struct TimerList {
  int64_t expiry;
  int64_t id;
  int64_t msecs;
  TzTimer *head, *tail;
  uint32_t position;
  int processing;
  int removed;
} TimerList;

#define TIMEOUT_MAX 2147483647.0

TimerList **tz_timer_heap;
size_t tz_timer_heap_size, tz_timer_heap_capacity;
TimerList **tz_timer_lists;
size_t tz_timer_list_count, tz_timer_list_capacity;
int64_t tz_timer_list_id;
int64_t tz_timer_refs;
TzTimer *tz_immediate_head, *tz_immediate_tail;
int64_t tz_immediate_refs;
const TzFrame *tz_main_await;

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

static void insert(TzTimer *t, double after, int64_t start) {
  int64_t msecs = (int64_t)after;
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
    after = 1;
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

void tz_timer_clear(TzTimer *t) {
  TZ_ORDERED();
  if (!t || t->immediate || t->cleared) {
    return;
  }
  t->cleared = 1;
  if (t->destroyed) {
    return;
  }
  t->destroyed = 1;
  TimerList *list = t->list;
  retain(t);
  unlink_timer(t);
  if (t->refed) {
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
  t->rc = 2;
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

static void list_on_timeout(TimerList *list, int64_t now) {
  int64_t msecs = list->msecs;
  int ran = 0;
  list->processing = 1;
  TzTimer *t;
  while ((t = list->head) != NULL) {
    if (now - t->idle_start < msecs) {
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
      continue;
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

typedef struct Wait {
  TzObject base;
  TzPromise *promise;
  int32_t done;
} Wait;

static void drop_pending_value(TzPromise *p) {
  const TzType *type = value_type(p);
  if (p->state != TZ_PROMISE_PENDING || !type) {
    return;
  }
  tz_value_release(type, value_of(p));
  memset(value_of(p), 0, type->size);
}

static void wait_drop(void *object) {
  TZ_ORDERED();
  Wait *w = (Wait *)object;
  drop_pending_value(w->promise);
  release(w->promise);
  tz_obj_free(w);
}

static const TzObjectDesc wait_desc = {wait_drop};

static void wait_fired(TzObject *env) {
  Wait *w = (Wait *)env;
  if (w->done) {
    return;
  }
  w->done = 1;
  tz_promise_fulfill(w->promise);
}

static const TzFunctionInfo wait_info = {(void *)wait_fired, {NULL, 0}};

void tz_timers_wait(TzPromise *p, double delay, int32_t immediate, int32_t ref) {
  TZ_ORDERED();
  Wait *w = (Wait *)tz_obj_alloc(sizeof(Wait));
  memset(w, 0, sizeof *w);
  w->base.rc = 1;
  w->base.desc = &wait_desc;
  w->promise = p;
  retain(p);
  TzTimer *timer = immediate ? tz_immediate_new(&wait_info, &w->base)
                             : tz_timer_new(&wait_info, &w->base, delay, 0);
  if (!ref) {
    tz_timer_ref(timer, 0);
  }
  release(timer);
}

void tz_async_iter_next(void *it, TzPromise *p) {
  TZ_ORDERED();
  ((const TzAsyncIterLayout *)((TzInstance *)it)->layout)->next(it, p);
}

void tz_async_iter_close(void *it, TzPromise *p) {
  TZ_ORDERED();
  ((const TzAsyncIterLayout *)((TzInstance *)it)->layout)->close(it, p);
}

static void fulfill_done(TzPromise *p) {
  p->flags |= TZ_PROMISE_DONE;
  tz_promise_fulfill(p);
}

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

enum { AGEN_START, AGEN_YIELD, AGEN_RUNNING, AGEN_RETURNING, AGEN_DONE };
_Static_assert(offsetof(TzAsyncGen, reactor) == 40 && sizeof(TzAsyncGen) == 80,
               "TzAsyncGen must match kAgenReactor and kAgenHeader of Lowering");

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

static void agen_resume(TzAsyncGen *g) {
  g->state = AGEN_RUNNING;
  retain(g);
  g->reactor.run(&g->reactor, NULL);
  release(g);
}

static void agen_drain(TzAsyncGen *g);

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

int32_t tz_agen_yield(TzAsyncGen *g, void *value, const TzType *type) {
  TZ_ORDERED();
  TzAgenStep *s = agen_pop(g);
  if (s) {
    const TzType *held = value_type(s->promise);
    if (held) {
      memcpy(value_of(s->promise), value, held->size);
    } else if (type && type->size) {
      tz_value_release(type, value);
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

int32_t tz_agen_resumed(TzAsyncGen *g) { return g->head ? g->head->close : 0; }

void tz_agen_finish(TzAsyncGen *g) {
  TZ_ORDERED();
  g->state = AGEN_DONE;
  TzAgenStep *s = agen_pop(g);
  if (s) {
    fulfill_done(s->promise);
    release(s->promise);
    tz_free(s);
  }
  agen_drain(g);
}

void tz_agen_drop_steps(TzAsyncGen *g) {
  while (g->head) {
    TzAgenStep *s = agen_pop(g);
    release(s->promise);
    tz_free(s);
  }
}

typedef struct {
  TzInstance base;
  TzTimer *timer;
  TzPromise *waiting;
  const TzType *type;
  double delay;
  int64_t fired;
  int32_t done;
  int32_t started, ref;
  uint8_t value[];
} Interval;

static void interval_drop(void *object);
static void interval_next(void *it, TzPromise *p);
static void interval_close(void *it, TzPromise *p);

const TzAsyncIterLayout tz_interval_layout = {
    {interval_drop, NULL, 0, TZ_LAYOUT_ASYNC_ITERATOR | TZ_LAYOUT_ASYNC_GENERATOR, NULL, 0, 0},
    interval_next,
    interval_close};

static void interval_give(Interval *it, TzPromise *p, int32_t turns) {
  const TzType *held = value_type(p);
  if (held) {
    memcpy(value_of(p), it->value, held->size);
    tz_value_retain(held, value_of(p));
  }
  fulfill_after(p, turns);
}

static void interval_stop(Interval *it) {
  it->done = 1;
  if (it->timer) {
    tz_timer_clear(it->timer);
    release(it->timer);
    it->timer = NULL;
  }
  TzPromise *p = it->waiting;
  it->waiting = NULL;
  if (p) {
    fulfill_done(p);
    release(p);
  }
}

static void interval_drop(void *object) {
  TZ_ORDERED();
  Interval *it = (Interval *)object;
  interval_stop(it);
  if (it->type) {
    tz_value_release(it->type, it->value);
  }
  tz_obj_free(it);
}

typedef struct {
  TzObject base;
  Interval *interval;
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

static void interval_start(Interval *it) {
  it->started = 1;
  IntervalTick *tick = (IntervalTick *)tz_obj_alloc(sizeof(IntervalTick));
  tick->base.rc = 1;
  tick->base.desc = &interval_tick_desc;
  tick->interval = it;
  it->timer = tz_timer_new(&interval_info, &tick->base, it->delay, 1);
  if (!it->ref) {
    tz_timer_ref(it->timer, 0);
  }
}

static void interval_next(void *object, TzPromise *p) {
  Interval *it = (Interval *)object;
  if (!it->started) {
    interval_start(it);
  }
  if (it->done) {
    fulfill_done(p);
    return;
  }
  if (it->fired > 0) {
    --it->fired;
    interval_give(it, p, 1);
    return;
  }
  if (it->waiting) {
    fulfill_done(p);
    return;
  }
  it->waiting = p;
  retain(p);
}

static void interval_close(void *object, TzPromise *p) {
  Interval *it = (Interval *)object;
  it->started = 1;
  interval_stop(it);
  fulfill_after(p, 1);
}

void *tz_timers_interval(double delay, const void *value, const TzType *type, int32_t ref) {
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

  return it;
}

void tz_async_main(TzPromise *main) {
  tz_async_run(1);
  if (main->state != TZ_PROMISE_PENDING) {
    return;
  }
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

void tz_async_shutdown(void) {
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
  tz_timer_heap = NULL;
  tz_timer_lists = NULL;
  tz_timer_heap_capacity = tz_timer_list_capacity = 0;
}
