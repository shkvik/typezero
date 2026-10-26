#include "runtime/library/events/signal/signal.h"
#include "runtime/core/error/error.h"
#include "runtime/core/group/group.h"
#include "runtime/core/parallel/parallel.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include <string.h>

extern TzErrorInfo tz_error_info;

static void release(void *object) { tz_instance_release((TzInstance *)object); }

static const TzType tz_signal_reason_type = {TZ_T_UNKNOWN, sizeof(void *), NULL};

static TzStr ascii(const char *text) { return tz_str_from_ascii(text, strlen(text)); }

int64_t tz_listen_order;

double tz_lib_order(TzMaker make) {
  (void)make;

  return (double)++tz_listen_order;
}

static void signal_drop(void *object) {
  TZ_ORDERED();
  TzSignal *s = (TzSignal *)object;
  if (s->reason) {
    tz_value_release_in(s, &tz_signal_reason_type, &s->reason);
  }
  tz_group_obj_free(s);
}

static const TzLayout tz_signal_layout = {signal_drop, NULL, 0, 0, "AbortSignal", 11, 0};

TzSignal *tz_signal_new(TzMaker make) {
  (void)make;
  TzSignal *s = (TzSignal *)tz_group_alloc(sizeof(TzSignal));
  int64_t rc = s->rc;
  memset(s, 0, sizeof *s);
  s->rc = rc;
  s->layout = &tz_signal_layout;

  return s;
}

void tz_signal_listen(TzSignal *s, TzAbortListener *l) {
  l->signal = s;
  l->order = ++tz_listen_order;
  l->next = NULL;
  l->prev = s->tail;
  if (s->tail) {
    s->tail->next = l;
  } else {
    s->head = l;
  }
  s->tail = l;
}

static void unlink_listener(TzSignal *s, TzAbortListener *l) {
  if (l->prev) {
    l->prev->next = l->next;
  } else if (s->head == l) {
    s->head = l->next;
  }
  if (l->next) {
    l->next->prev = l->prev;
  } else if (s->tail == l) {
    s->tail = l->prev;
  }
  l->next = l->prev = NULL;
  l->signal = NULL;
}

void tz_signal_unlisten(TzAbortListener *l) {
  if (l->signal) {
    unlink_listener(l->signal, l);
  }
}

int32_t tz_signal_aborted(const void *signal) {
  return signal && ((const TzSignal *)signal)->aborted;
}

void tz_lib_signal_abort(TzSignal *s, void *reason, TzMaker make) {
  (void)make;
  if (s->aborted) {
    return;
  }
  s->aborted = 1;
  if (reason) {
    tz_ref_retain(reason);
    s->reason = reason;
    tz_group_store(s, reason);
  }
}

void tz_lib_signal_run(TzSignal *s, double order, TzMaker make) {
  (void)make;
  tz_ref_retain(s);
  while (s->head && (double)s->head->order < order) {
    TzAbortListener *l = s->head;
    unlink_listener(s, l);
    l->run(l);
  }
  release(s);
}

void *tz_unknown_error(void *u);

void *tz_signal_reason(const void *signal) {
  void *reason = ((const TzSignal *)signal)->reason;
  if (reason) {
    tz_ref_retain(reason);
  }

  return tz_unknown_error(reason);
}

void *tz_abort_error(const void *signal) {
  TzStr text = ascii("The operation was aborted");
  void *error = tz_error_info.make(TZ_ERROR, text.obj, text.meta);
  TzStr *name = (TzStr *)((char *)error + tz_error_info.name_offset);
  tz_str_release_value(*name);
  *name = ascii("AbortError");
  void *reason = ((const TzSignal *)signal)->reason;
  if (reason && (((TzInstance *)reason)->layout->flags & TZ_LAYOUT_ERROR)) {
    tz_ref_retain(reason);
    *(void **)((char *)error + tz_error_info.cause_offset) = reason;
  }

  return error;
}
