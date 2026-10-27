#ifndef TZ_LIBRARY_H
#define TZ_LIBRARY_H

#include "runtime/core/string/string.h"
#include <stddef.h>
#include <stdint.h>

void *tz_lib_bare(void *map, void *(*make)(const void *));
void tz_lib_class_name(TzStr *out, const void *o, void *(*make)(const void *));
int64_t tz_lib_pid(void *(*make)(const void *));
void *tz_defer_new(const void *info, void *env);

#endif
