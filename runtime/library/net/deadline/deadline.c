#include "runtime/library/net/deadline/deadline.h"
#include "runtime/core/group/group.h"
#include "runtime/core/parallel/parallel.h"
#include "runtime/core/tzrt/tzrt.h"
#include <string.h>

static void release(void *object) { tz_instance_release((TzInstance *)object); }

static void deadline_drop(void *object) {
  TZ_ORDERED();
  tz_group_obj_free(object);
}

static const TzLayout tz_deadline_layout = {deadline_drop, NULL, 0, 0, "Deadline", 8, 0};

TzDeadline *tz_deadline_new(TzMaker make) {
  (void)make;
  TzDeadline *d = (TzDeadline *)tz_group_alloc(sizeof(TzDeadline));
  int64_t rc = d->rc;
  memset(d, 0, sizeof *d);
  d->rc = rc;
  d->layout = &tz_deadline_layout;

  return d;
}

void tz_deadline_listen(TzDeadline *d, TzDeadlineListener *l) {
  l->deadline = d;
  l->next = NULL;
  l->prev = d->tail;
  if (d->tail) {
    d->tail->next = l;
  } else {
    d->head = l;
  }
  d->tail = l;
}

static void unlink_listener(TzDeadline *d, TzDeadlineListener *l) {
  if (l->prev) {
    l->prev->next = l->next;
  } else if (d->head == l) {
    d->head = l->next;
  }
  if (l->next) {
    l->next->prev = l->prev;
  } else if (d->tail == l) {
    d->tail = l->prev;
  }
  l->next = l->prev = NULL;
  l->deadline = NULL;
}

void tz_deadline_unlisten(TzDeadlineListener *l) {
  if (l->deadline) {
    unlink_listener(l->deadline, l);
  }
}

int32_t tz_deadline_is_passed(const void *deadline) {
  return deadline && ((const TzDeadline *)deadline)->passed;
}

int32_t tz_deadline_passed(TzDeadline *d, TzMaker make) {
  (void)make;

  return d->passed;
}

void tz_deadline_pass(TzDeadline *d, TzMaker make) {
  (void)make;
  if (d->passed) {
    return;
  }
  d->passed = 1;
  tz_ref_retain(d);
  while (d->head) {
    TzDeadlineListener *l = d->head;
    unlink_listener(d, l);
    l->run(l);
  }
  release(d);
}
