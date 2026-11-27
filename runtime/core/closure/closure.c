#include "runtime/core/closure/closure.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/parallel/parallel.h"
#include "runtime/core/tzrt/tzrt.h"

int64_t tz_obj_live_objects(void) { return tz_thread_sum(offsetof(TzThread, live_objects)); }

static int64_t tz_immortal_objects_count = 0;

int64_t tz_obj_immortal_objects(void) { return tz_immortal_objects_count; }

void tz_obj_forget(void) {
  --tz_live_objects;
  ++tz_immortal_objects_count;
}

TZ_HOT void *tz_obj_alloc(int64_t size) {
  ++tz_live_objects;

  return tz_alloc((size_t)size);
}

TZ_HOT void tz_obj_free(void *object) {
  --tz_live_objects;
  tz_free(object);
}
