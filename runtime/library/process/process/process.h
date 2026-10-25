// The process and the clock.

#ifndef TZ_PROCESS_H
#define TZ_PROCESS_H

#include "runtime/core/string/string.h"
#include <stddef.h>
#include <stdint.h>

// process.env (process.c): the variable of a live entry changed, or one was
// removed.
void tz_process_env_changed(const TzStr *name, const TzStr *value);

// Programs call these functions, and console.error and process.stdout.write
// those of output.c, through Lowering::processFunction
// (compiler/library/process/codegen/codegen_process.cpp), which declares each
// where it is used.

// The code the program ends with when its code runs to the end:
// process.exitCode, or 0.
int32_t tz_process_exit_status(void);
// Releases what the runtime holds, before the leak check.
void tz_process_shutdown(void);
// Notes when the program starts, for performance.now().
void tz_clock_init(void);
// process.cwd(), and process.env[name]: whether it is set.
void tz_process_cwd(TzStr *out);
int32_t tz_process_env(TzStr *out, void *obj, uint64_t meta);

#endif
