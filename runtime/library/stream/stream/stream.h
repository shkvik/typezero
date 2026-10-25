// What the streams of tz's library (library/stream/) need of the runtime.

#ifndef TZ_STREAM_H
#define TZ_STREAM_H

#include "runtime/core/tzrt/tzrt.h"

// `__tz_chunk<T>(u)` of an unknown that holds no T (stream.c): ends the
// program.
TZ_COLD TZ_NORETURN void tz_lib_bad_chunk(const void *u, const char *type, uint32_t type_length);
// Whether the unknown `u` (borrowed) holds binary data of `kind` (TZ_BYTES_*).
int32_t tz_lib_is_bytes(const void *u, int32_t kind);

#endif
