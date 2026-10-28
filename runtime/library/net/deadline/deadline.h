#ifndef TZ_DEADLINE_H
#define TZ_DEADLINE_H

#include "runtime/core/tzrt/tzrt.h"

typedef struct TzDeadlineListener {
  struct TzDeadlineListener *next, *prev;
  struct TzDeadline *deadline;
  void (*run)(struct TzDeadlineListener *self);
} TzDeadlineListener;

typedef struct TzDeadline {
  int64_t rc;
  const TzLayout *layout;
  uint8_t passed;
  TzDeadlineListener *head, *tail;
} TzDeadline;

int32_t tz_deadline_is_passed(const void *deadline);
void tz_deadline_listen(TzDeadline *d, TzDeadlineListener *l);
void tz_deadline_unlisten(TzDeadlineListener *l);

#endif
