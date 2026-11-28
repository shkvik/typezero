#ifndef TZ_PROCESS_H
#define TZ_PROCESS_H

#include "runtime/core/string/string.h"
#include <stddef.h>
#include <stdint.h>

void tz_process_env_changed(const TzStr *name, const TzStr *value);

void tz_process_shutdown(void);
void tz_clock_init(void);

#endif
