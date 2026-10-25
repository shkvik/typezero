// Buffered output for console.log, console.error and process.stdout.write, the text
// of numbers, panics, and the stack of each thread.

#ifndef TZ_OUTPUT_H
#define TZ_OUTPUT_H

#include "runtime/core/tzrt/tzrt.h"

// Buffered stdout.
void tz_out_write(const char *data, size_t len);
// Bytes as they are to stdout (fd 1) or stderr (2), in order with
// console.log: writeFileSync(1, bytes).
void tz_std_write_bytes(int32_t fd, const uint8_t *data, size_t len);

// Formats a double like JavaScript's Number#toString; returns the length.
// `out` must hold at least 32 bytes.
size_t tz_format_f64(double value, char *out);

TZ_COLD TZ_NORETURN void tz_panic(const char *message, int64_t len);
TZ_COLD TZ_NORETURN void tz_panicf(const char *format, ...);

// Sets tz_stack_limit, and the room the handler of a stack overflow needs,
// for the thread that calls it.
void tz_thread_stack_init(void);

// stdout and stderr: process.stdout.write and the like, and the
// output buffer written out (with `all` unset, its complete characters).
int32_t tz_stdout_write(void *obj, uint64_t meta);
int32_t tz_stderr_write(void *obj, uint64_t meta);
void tz_out_flush(int all);

#endif
