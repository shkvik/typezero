#ifndef TZ_PROCESS_H
#define TZ_PROCESS_H

#include "runtime/core/string/string.h"
#include <stddef.h>
#include <stdint.h>

void tz_process_env_changed(const TzStr *name, const TzStr *value);

int32_t tz_process_exit_status(void);
void tz_process_shutdown(void);
void tz_clock_init(void);
void tz_process_cwd(TzStr *out);
int32_t tz_process_env(TzStr *out, void *obj, uint64_t meta);

#endif
