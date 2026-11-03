#ifndef TZ_LIBRARY_H
#define TZ_LIBRARY_H

#include "runtime/core/string/string.h"
#include <stddef.h>
#include <stdint.h>

void *tz_lib_bare(void *map, void *(*make)(const void *));
void tz_lib_class_name(TzStr *out, const void *o, void *(*make)(const void *));
int64_t tz_lib_pid(void *(*make)(const void *));
void *tz_defer_new(const void *info, void *env);
void *tz_listener_code(void *listener, void *(*make)(const void *));
void *tz_listener_weak(void *listener, void *(*make)(const void *));
void *tz_bound_listener(void *code, void *weak, void *(*make)(const void *));
void *tz_weak_target(void *weak, void *(*make)(const void *));
int32_t tz_weak_is(void *weak, void *listener, void *(*make)(const void *));
int32_t tz_same_code(void *a, void *b, void *(*make)(const void *));
int32_t tz_weak_alive(void *weak, void *(*make)(const void *));

#endif
