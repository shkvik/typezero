#ifndef TZ_CSTRING_H
#define TZ_CSTRING_H

#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"

typedef struct TzCString {
  int64_t rc;
  const TzLayout *layout;
  int64_t length;
  char text[];
} TzCString;

extern const TzLayout tz_cstring_layout;

void tz_cstring_to_str(TzStr *out, const void *cstring);
int32_t tz_cstring_equal(const void *a, const void *b);

static inline int tz_is_cstring(const TzInstance *object) {
  return object && (object->layout->flags & TZ_LAYOUT_CSTRING) != 0;
}

#endif
