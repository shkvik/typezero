#ifndef TZ_JSON_VALUES_H
#define TZ_JSON_VALUES_H

#include "runtime/core/string/string.h"
#include "runtime/library/json/json/json.h"
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

TzJson *tz_json_string_node(TzStr s);

TzJson *tz_json_array_node(uint32_t capacity);

TzJson *tz_json_object_new(void);

const char *tz_json_kind_name(const TzJson *j);

#endif
