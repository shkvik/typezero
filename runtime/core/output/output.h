#ifndef TZ_OUTPUT_H
#define TZ_OUTPUT_H

#include "runtime/core/tzrt/tzrt.h"

void tz_out_write(const char *data, size_t len);
void tz_std_write_bytes(int32_t fd, const uint8_t *data, size_t len);

size_t tz_format_f64(double value, char *out);

TZ_COLD TZ_NORETURN void tz_panic(const char *message, int64_t len);
TZ_COLD TZ_NORETURN void tz_panicf(const char *format, ...);
TZ_COLD TZ_NORETURN void tz_panic_str(void *obj, uint64_t meta);

void tz_thread_stack_init(void);

int32_t tz_stdout_write(void *obj, uint64_t meta);
int32_t tz_stderr_write(void *obj, uint64_t meta);
void tz_out_flush(int all);
void tz_out_idle(void);
void tz_out_rescue(void);

#endif
