// util.inspect, as console.log prints values.

#ifndef TZ_INSPECT_H
#define TZ_INSPECT_H

#include "runtime/core/string/string.h"
#include <stddef.h>
#include <stdint.h>

// util.inspect of the unknown `u` (borrowed), a string quoted (inspect.c).
void tz_lib_inspect(TzStr *out, const void *u, void *(*make)(const void *));

#endif
