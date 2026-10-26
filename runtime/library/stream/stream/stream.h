#ifndef TZ_STREAM_H
#define TZ_STREAM_H

#include "runtime/core/tzrt/tzrt.h"

TZ_COLD TZ_NORETURN void tz_lib_bad_chunk(const void *u, const char *type, uint32_t type_length);
int32_t tz_lib_is_bytes(const void *u, int32_t kind);

#endif
