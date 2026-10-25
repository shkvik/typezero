// Metadata of decorators (docs/decorators.md).

#ifndef TZ_META_H
#define TZ_META_H

#include "runtime/core/unknown/unknown.h"
#include <stddef.h>
#include <stdint.h>

// Metadata (meta.c): records by key (an object key's address, or NULL and
// the string), target and property (has_property unset for none). Values
// are unknowns; define takes `value` over, get gives a new reference or NULL.
void tz_meta_init(TzClass *const *classes, uint32_t count);
// `compiler` is set for a record `design:*`, which the program cannot write.
void tz_meta_define(const void *key, void *key_obj, uint64_t key_meta, void *value, void *target,
                    void *property_obj, uint64_t property_meta, int32_t has_property,
                    int32_t compiler);
void *tz_meta_get(const void *key, void *key_obj, uint64_t key_meta, const void *target,
                  void *property_obj, uint64_t property_meta, int32_t has_property, int32_t own);
int32_t tz_meta_has(const void *key, void *key_obj, uint64_t key_meta, const void *target,
                    void *property_obj, uint64_t property_meta, int32_t has_property, int32_t own);
int32_t tz_meta_delete(const void *key, void *key_obj, uint64_t key_meta, const void *target,
                       void *property_obj, uint64_t property_meta, int32_t has_property);
// Releases what the records hold, before the leak check.
void tz_meta_shutdown(void);

#endif
