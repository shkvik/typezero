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

void *tz_json_out_begin(void);
void tz_json_out_end(void *out, TzStr *text);
void tz_json_out_grow(TzJsonOut *o, size_t n);
void tz_json_out_raw(void *out, const char *text, int64_t length);
void tz_json_out_raw_slow(TzJsonOut *o, const char *text, int64_t length);
void tz_json_out_str(void *out, void *obj, uint64_t meta);
void tz_json_out_str_slow(TzJsonOut *o, void *obj, uint64_t meta);
void tz_json_out_int(void *out, int64_t value);
void tz_json_out_uint(void *out, uint64_t value);
void tz_json_out_double(void *out, double value);
int32_t tz_json_out_value(void *out, const TzType *type, const void *value);

#endif
