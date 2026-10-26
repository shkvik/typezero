#ifndef TZ_META_H
#define TZ_META_H

#include "runtime/core/unknown/unknown.h"
#include <stddef.h>
#include <stdint.h>

void tz_meta_init(TzClass *const *classes, uint32_t count);
void tz_meta_define(const void *key, void *key_obj, uint64_t key_meta, void *value, void *target,
                    void *property_obj, uint64_t property_meta, int32_t has_property,
                    int32_t compiler);
void *tz_meta_get(const void *key, void *key_obj, uint64_t key_meta, const void *target,
                  void *property_obj, uint64_t property_meta, int32_t has_property, int32_t own);
int32_t tz_meta_has(const void *key, void *key_obj, uint64_t key_meta, const void *target,
                    void *property_obj, uint64_t property_meta, int32_t has_property, int32_t own);
int32_t tz_meta_delete(const void *key, void *key_obj, uint64_t key_meta, const void *target,
                       void *property_obj, uint64_t property_meta, int32_t has_property);
void tz_meta_shutdown(void);

#endif
