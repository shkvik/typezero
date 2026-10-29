#ifndef TZ_SIGNALS_H
#define TZ_SIGNALS_H

#include "runtime/core/tzrt/tzrt.h"
#include <stdint.h>

void tz_signal_init(void);
void tz_signal_shutdown(void);
void tz_signal_listen(const void *info, void *env, TzMaker make);
void tz_signal_watch(int64_t signal, int32_t watched, TzMaker make);

#endif
