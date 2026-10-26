#ifndef TZ_INSPECT_H
#define TZ_INSPECT_H

#include "runtime/core/string/string.h"
#include <stddef.h>
#include <stdint.h>

void tz_lib_inspect(TzStr *out, const void *u, void *(*make)(const void *));

#endif
