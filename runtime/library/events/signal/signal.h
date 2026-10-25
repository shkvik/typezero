// The runtime's side of an AbortSignal.

#ifndef TZ_SIGNAL_H
#define TZ_SIGNAL_H

#include "runtime/core/tzrt/tzrt.h"

// A listener of the runtime's own to the abort of a signal: what waits and
// may be cancelled. `run` gets it once, unlinked already. `order` places it
// among the listeners of the program (see signal.c).
typedef struct TzAbortListener {
  struct TzAbortListener *next, *prev;
  struct TzSignal *signal; // the one it listens to, or NULL
  int64_t order;
  void (*run)(struct TzAbortListener *self);
} TzAbortListener;

// What an AbortSignal of tz's library (library/events/web.tz) holds for the
// runtime: whether it is aborted, the reason, and the runtime's listeners.
typedef struct TzSignal {
  int64_t rc;
  const TzLayout *layout;
  uint8_t aborted;
  void *reason; // an unknown, NULL before the abort or for undefined
  TzAbortListener *head, *tail;
} TzSignal;

int32_t tz_signal_aborted(const void *signal);
void tz_signal_listen(TzSignal *s, TzAbortListener *l);
void tz_signal_unlisten(TzAbortListener *l);
// A new AbortError whose cause is the reason of the signal, as Node rejects
// what a signal cancelled with.
void *tz_abort_error(const void *signal);
// The reason itself, owned, as fetch rejects with it: an Error, or the
// TypeError that throwing what it is gives.
void *tz_signal_reason(const void *signal);

#endif
