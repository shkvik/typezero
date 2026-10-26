#ifndef TZ_PUBLISH_H
#define TZ_PUBLISH_H

#include "runtime/core/tzrt/tzrt.h"

void tz_publish_value(const TzType *type, void *value);
void tz_publish_object(void *object);
void tz_store_barrier(void *owner, const TzType *type, void *value);
void tz_store_barrier_ref(void *owner, void *object);

#endif
