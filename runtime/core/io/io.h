#ifndef TZ_IO_H
#define TZ_IO_H

#include <stddef.h>
#include <stdint.h>

void tz_io_wake_loop(void);
void tz_work_ref_count(int32_t n);

typedef struct TzWork {
  struct TzWork *next;
  void (*work)(struct TzWork *self);
  void (*done)(struct TzWork *self);
  uint32_t turns;
  uint32_t unref;
  uint64_t queued, due;
  struct TzPromise *settles;
} TzWork;

void tz_work_queue(TzWork *w);
void tz_work_queue_settles(TzWork *w, struct TzPromise *p);
void tz_io_begin(TzWork *w);
void tz_io_due(TzWork *w);
void *tz_io_port_handle(void);
void tz_work_unref(TzWork *w);
void tz_work_ref(TzWork *w);
int tz_io_alive(void);
void tz_io_poll(int64_t timeout);
extern void (*tz_io_sender)(void);
void tz_io_send_pending(void);

#endif
