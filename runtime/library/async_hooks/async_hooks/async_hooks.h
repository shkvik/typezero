#ifndef TZ_ASYNC_HOOKS_H
#define TZ_ASYNC_HOOKS_H

#include "runtime/core/tzrt/tzrt.h"

void *tz_context_current(TzMaker make);
void *tz_context_enter(void *frame);
void tz_context_leave(void *outer);

#endif
