#ifndef TZ_OUTPUT_H
#define TZ_OUTPUT_H

#include "runtime/core/tzrt/tzrt.h"

void tz_std_write_bytes(int32_t fd, const uint8_t *data, size_t len);
int32_t tz_std_gone(int32_t fd);



void tz_thread_stack_init(void);

int32_t tz_stdout_write(void *obj, uint64_t meta);
int32_t tz_stderr_write(void *obj, uint64_t meta);

#endif
