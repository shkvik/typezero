#ifndef TZ_SIGNAL_H
#define TZ_SIGNAL_H

#include "runtime/core/tzrt/tzrt.h"

typedef struct TzAbortListener {
  struct TzAbortListener *next, *prev;
  struct TzSignal *signal;
  int64_t order;
  void (*run)(struct TzAbortListener *self);
} TzAbortListener;

typedef struct TzSignal {
  int64_t rc;
  const TzLayout *layout;
  uint8_t aborted;
  void *reason;
  TzAbortListener *head, *tail;
} TzSignal;

int32_t tz_signal_aborted(const void *signal);
void tz_signal_listen(TzSignal *s, TzAbortListener *l);
void tz_signal_unlisten(TzAbortListener *l);

#endif
