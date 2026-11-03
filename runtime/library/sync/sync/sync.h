#ifndef TZ_SYNC_H
#define TZ_SYNC_H

#include "runtime/core/async/async.h"
#include "runtime/core/tzrt/tzrt.h"

void *tz_sync_guard(TzMaker make);
void tz_sync_read(void *guard, TzMaker make);
void tz_sync_read_done(void *guard, TzMaker make);
void tz_sync_write(void *guard, TzMaker make);
void tz_sync_write_done(void *guard, TzMaker make);
void *tz_sync_waiter(TzMaker make);
void tz_sync_wait(TzPromise *p, void *waiter, TzMaker make);
void tz_sync_wake(void *waiter, TzMaker make);

#endif
