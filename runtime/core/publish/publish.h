// Shared objects (docs/async.md, section 8).

#ifndef TZ_PUBLISH_H
#define TZ_PUBLISH_H

#include "runtime/core/tzrt/tzrt.h"

// Shared objects (publish.c, docs/async.md, section 8): everything a value
// of `type` at `value` holds, or an object (with a descriptor or a layout)
// holds, and they, become shared, counted atomically from then on, before
// other threads may reach them. After a store of such a value into `owner`,
// tz_store_barrier publishes it when `owner` is shared. All of it does
// nothing in the runtime of one thread.
void tz_publish_value(const TzType *type, void *value);
void tz_publish_object(void *object);
void tz_store_barrier(void *owner, const TzType *type, void *value);
void tz_store_barrier_ref(void *owner, void *object);

#endif
