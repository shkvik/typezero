#ifndef TZ_JSON_STRINGIFY_H
#define TZ_JSON_STRINGIFY_H

#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include <stddef.h>
#include <stdint.h>

typedef struct TzJsonOut {
  uint8_t *data;
  size_t length, capacity;
  int two;
  uint64_t high;
} TzJsonOut;

extern TZ_THREAD_LOCAL uint8_t *tz_json_scratch;
extern TZ_THREAD_LOCAL size_t tz_json_scratch_capacity;
extern TZ_THREAD_LOCAL TzJsonOut tz_json_out_state;

void tz_json_out_str(void *out, void *obj, uint64_t meta);

#endif
