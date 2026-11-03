#include "runtime/library/tz_modules/library/library.h"
#include "runtime/core/closure/closure.h"
#include "runtime/core/group/group.h"
#include "runtime/core/map/map.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include "runtime/core/unknown/unknown.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

void *tz_lib_bare(void *map, TzMaker make) {
  (void)make;
  ((TzMap *)map)->flags |= TZ_MAP_BARE;
  tz_ref_retain(map);

  return map;
}

void tz_lib_class_name(TzStr *out, const void *o, TzMaker make) {
  (void)make;
  const TzLayout *layout = ((const TzInstance *)o)->layout;
  if (layout->flags & TZ_LAYOUT_BOX) {
    *out = tz_str_from_ascii("Object", 6);
    return;
  }
  if (!layout->name_length) {
    *out = tz_str_from_ascii("Object", 6);
    return;
  }
  TzBuf b;
  tz_buf_init(&b);
  for (uint32_t i = 0; i < layout->name_length; ++i) {
    tz_buf_push(&b, (uint8_t)layout->name[i]);
  }
  *out = tz_buf_take(&b);
}

typedef struct TzDefer {
  int64_t rc;
  const TzLayout *layout;
  TzFunction run;
} TzDefer;

static void defer_drop(void *object) {
  TzDefer *d = (TzDefer *)object;
  TzFunction run = d->run;
  ((void (*)(TzObject *))run.info->code)(run.env);
  tz_obj_release(run.env);
  tz_obj_free(d);
}

static const TzLayout defer_layout = {defer_drop, NULL, 0, 0, "Defer", 5, 0};

void *tz_defer_new(const void *info, void *env) {
  TzDefer *d = (TzDefer *)tz_obj_alloc(sizeof(TzDefer));
  d->rc = 1;
  d->layout = &defer_layout;
  d->run.info = (const TzFunctionInfo *)info;
  d->run.env = (TzObject *)env;

  return d;
}

int64_t tz_lib_pid(TzMaker make) {
  (void)make;
#ifdef _WIN32

  return (int32_t)GetCurrentProcessId();
#else

  return (int32_t)getpid();
#endif
}

static const TzLayout listener_weak_layout = {tz_weak_drop, NULL, 0, 0, "WeakRef", 7, 0};

static const TzFunction *boxed_function(const void *box) {
  return (const TzFunction *)TZ_BOX_VALUE(box);
}

void *tz_listener_code(void *listener, TzMaker make) {
  (void)make;
  TzFunction f = *boxed_function(listener);
  f.env = NULL;

  return tz_unknown_box(((const TzBox *)listener)->type, &f);
}

void *tz_listener_weak(void *listener, TzMaker make) {
  (void)make;

  return tz_weak_new(&listener_weak_layout, boxed_function(listener)->env);
}

void *tz_bound_listener(void *code, void *weak, TzMaker make) {
  (void)make;
  void *target = tz_weak_deref(weak);
  if (!target) {
    return NULL;
  }
  TzFunction f = *boxed_function(code);
  f.env = (TzObject *)target;

  return tz_unknown_box(((const TzBox *)code)->type, &f);
}

void *tz_weak_target(void *weak, TzMaker make) {
  (void)make;

  return tz_weak_deref(weak);
}

int32_t tz_weak_is(void *weak, void *listener, TzMaker make) {
  (void)make;
  void *target = tz_weak_peek(weak);

  return target && target == (void *)boxed_function(listener)->env;
}

int32_t tz_same_code(void *a, void *b, TzMaker make) {
  (void)make;

  return boxed_function(a)->info == boxed_function(b)->info;
}

int32_t tz_weak_alive(void *weak, TzMaker make) {
  (void)make;

  return tz_weak_peek(weak) != NULL;
}
