// Waiting for I/O: the thread pool and the poll phase of the loop.

#ifndef TZ_IO_H
#define TZ_IO_H

#include <stddef.h>
#include <stdint.h>

// Wakes the poll phase of the loop from another thread (io.c).
void tz_io_wake_loop(void);
// The loop waits for `n` more things of the runtime's own (io.c).
void tz_work_ref_count(int32_t n);

// What the runtime gives the thread pool. `work` runs on a thread of the
// pool: calls of the system only, into memory of its own (malloc), never the
// allocator of the runtime or an object of the program, which are not
// thread-safe; NULL for something that only waits for its turn of the loop.
// `done` runs on the main thread in the poll phase, as a callback of Node
// does: it settles what the work is for and frees it. `turns` is how many
// turns of the loop Node takes for the same thing at the least (its round
// trips to the pool, 1 for 0), which the work may set too; `done` runs no
// earlier than that many polls after the work was queued. A work that is
// `unref` does not keep the loop alive.
//
// `settles`: the promise that `done` settles, when settling it is all that
// `done` does that the program can see (tz_work_queue_settles): the parallel
// runtime may then run what waits for it ahead of the event (parallel.c).
typedef struct TzWork {
  struct TzWork *next;
  void (*work)(struct TzWork *self);
  void (*done)(struct TzWork *self);
  uint32_t turns;
  uint32_t unref;
  uint64_t queued, due; // of the poll phases
  struct TzPromise *settles;
} TzWork;

void tz_work_queue(TzWork *w);
void tz_work_queue_settles(TzWork *w, struct TzPromise *p);
// Waits of the runtime's own, outside the pool (net.c): a work that began
// waiting keeps the loop alive (unless unref) until it is due; once what it
// waited for came, tz_io_due hands it to the poll phase, which runs `done`.
// On Windows the system posts it to the completion port of the loop, which
// tz_io_port_handle gives, made if need be.
void tz_io_begin(TzWork *w);
void tz_io_due(TzWork *w);
void *tz_io_port_handle(void);
// The work stops keeping the loop alive (it may still be done), or starts
// again.
void tz_work_unref(TzWork *w);
void tz_work_ref(TzWork *w);
// For the loop (async.c): whether queued work keeps it alive, and the poll
// phase, which waits up to `timeout` ms for the pool (-1 without a limit).
int tz_io_alive(void);
void tz_io_poll(int64_t timeout);

#endif
