#include "runtime/core/closure/closure.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/parallel/parallel.h"
#include "runtime/core/tzrt/tzrt.h"

TZ_HOT void *tz_obj_alloc(int64_t size) {
  ++tz_live_objects;

  return tz_alloc((size_t)size);
}

TZ_HOT void tz_obj_free(void *object) {
  --tz_live_objects;
  tz_free(object);
}
