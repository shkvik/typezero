// The objects of closures, environments and the boxes of shared variables
// (see TzObject in closure.h), and the objects of the language (TzInstance).
// The compiler lays them out and releases what they hold; the runtime only
// allocates them and counts them for leak checks.

#include "runtime/core/closure/closure.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/parallel/parallel.h"
#include "runtime/core/tzrt/tzrt.h"

int64_t tz_obj_live_objects(void) { return tz_thread_sum(offsetof(TzThread, live_objects)); }

TZ_HOT void *tz_obj_alloc(int64_t size) {
  ++tz_live_objects;

  return tz_alloc((size_t)size);
}

TZ_HOT void tz_obj_free(void *object) {
  --tz_live_objects;
  tz_free(object);
}
