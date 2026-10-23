// The objects of closures: environments and the boxes of shared variables
// (see TzObject in tzrt.h). The compiler lays them out and releases what they
// hold; the runtime only allocates them and counts them for leak checks.

#include "tzrt.h"

int64_t tz_live_objects; // not static: see the rules in tzrt.h

int64_t tz_obj_live_objects(void) { return tz_live_objects; }

TZ_HOT void *tz_obj_alloc(int64_t size) {
  ++tz_live_objects;
  return tz_alloc((size_t)size);
}

TZ_HOT void tz_obj_free(void *object) {
  --tz_live_objects;
  tz_free(object);
}
