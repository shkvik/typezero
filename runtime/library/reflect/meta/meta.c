#include "runtime/library/reflect/meta/meta.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/error/error.h"
#include "runtime/core/group/group.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include "runtime/core/unknown/unknown.h"
#include <string.h>

typedef struct {
  const void *key;
  TzStr text;
  void *target;
  TzStr property;
  int32_t has_property;
  void *value;
} TzMetaEntry;

typedef struct {
  TzMetaEntry *entries;
  uint32_t count, capacity;
  TzClass *const *classes;
  uint32_t class_count;
} TzMetaStore;

TzMetaStore tz_meta_store;

void tz_meta_init(TzClass *const *classes, uint32_t count) {
  tz_meta_store.classes = classes;
  tz_meta_store.class_count = count;
}

static TzStr flat(void *obj, uint64_t meta) {
  TzStr s = {obj, meta};
  uint32_t length;
  int two;
  tz_str_units(&s, &length, &two);

  return s;
}

static int same_string(TzStr a, void *obj, uint64_t meta) {
  return tz_str_equal(a.obj, a.meta, obj, meta) != 0;
}

static TzMetaEntry *find(const void *key, void *key_obj, uint64_t key_meta, const void *target,
                         void *property_obj, uint64_t property_meta, int32_t has_property) {
  for (uint32_t i = 0; i < tz_meta_store.count; ++i) {
    TzMetaEntry *e = &tz_meta_store.entries[i];
    if (e->target != target || e->key != key || e->has_property != has_property) {
      continue;
    }
    if (!key && !same_string(e->text, key_obj, key_meta)) {
      continue;
    }
    if (has_property && !same_string(e->property, property_obj, property_meta)) {
      continue;
    }

    return e;
  }

  return NULL;
}

static const void *up(const void *target) {
  const TzInstance *o = (const TzInstance *)target;
  if (o->layout->flags & TZ_LAYOUT_CLASS) {
    return ((const TzClass *)target)->parent;
  }
  uint32_t id = o->layout->class_id;
  if (id && id < tz_meta_store.class_count) {
    return tz_meta_store.classes[id];
  }

  return NULL;
}

void tz_meta_define(const void *key, void *key_obj, uint64_t key_meta, void *value, void *target,
                    void *property_obj, uint64_t property_meta, int32_t has_property,
                    int32_t compiler) {
  if (!key && !compiler) {
    TzStr text = flat(key_obj, key_meta);
    uint32_t length;
    int two;
    const void *units = tz_str_units(&text, &length, &two);
    static const char design[] = "design:";
    int reserved = length >= 7;
    for (uint32_t i = 0; i < 7 && reserved; ++i) {
      reserved =
          (two ? ((const uint16_t *)units)[i] : ((const uint8_t *)units)[i]) == (uint8_t)design[i];
    }
    if (reserved) {
      tz_instance_release((TzInstance *)value);
      return;
    }
  }
  TzMetaEntry *e = find(key, key_obj, key_meta, target, property_obj, property_meta, has_property);
  if (e) {
    tz_instance_release((TzInstance *)e->value);
    e->value = value;
    return;
  }
  if (tz_meta_store.count == tz_meta_store.capacity) {
    uint32_t capacity = tz_meta_store.capacity ? tz_meta_store.capacity * 2 : 16;
    TzMetaEntry *entries = (TzMetaEntry *)tz_alloc(capacity * sizeof(TzMetaEntry));
    if (tz_meta_store.count) {
      memcpy(entries, tz_meta_store.entries, tz_meta_store.count * sizeof(TzMetaEntry));
    }
    tz_free(tz_meta_store.entries);
    tz_meta_store.entries = entries;
    tz_meta_store.capacity = capacity;
  }
  e = &tz_meta_store.entries[tz_meta_store.count++];
  memset(e, 0, sizeof *e);
  e->key = key;
  if (!key) {
    e->text = flat(key_obj, key_meta);
    tz_str_retain_value(e->text);
  }
  e->target = target;
  tz_ref_retain(target);
  e->has_property = has_property;
  if (has_property) {
    e->property = flat(property_obj, property_meta);
    tz_str_retain_value(e->property);
  }
  e->value = value;
}

static TzMetaEntry *lookup(const void *key, void *key_obj, uint64_t key_meta, const void *target,
                           void *property_obj, uint64_t property_meta, int32_t has_property,
                           int32_t own) {
  for (const void *t = target; t; t = own ? NULL : up(t)) {
    TzMetaEntry *e = find(key, key_obj, key_meta, t, property_obj, property_meta, has_property);
    if (e) {
      return e;
    }
  }

  return NULL;
}

void *tz_meta_get(const void *key, void *key_obj, uint64_t key_meta, const void *target,
                  void *property_obj, uint64_t property_meta, int32_t has_property, int32_t own) {
  TzMetaEntry *e =
      lookup(key, key_obj, key_meta, target, property_obj, property_meta, has_property, own);
  if (!e || !e->value) {
    return NULL;
  }
  tz_ref_retain(e->value);

  return e->value;
}

int32_t tz_meta_has(const void *key, void *key_obj, uint64_t key_meta, const void *target,
                    void *property_obj, uint64_t property_meta, int32_t has_property, int32_t own) {
  return lookup(key, key_obj, key_meta, target, property_obj, property_meta, has_property, own) !=
         NULL;
}

static void release_entry(TzMetaEntry *e) {
  tz_instance_release((TzInstance *)e->value);
  tz_instance_release((TzInstance *)e->target);
  if (!e->key) {
    tz_str_release_value(e->text);
  }
  if (e->has_property) {
    tz_str_release_value(e->property);
  }
}

int32_t tz_meta_delete(const void *key, void *key_obj, uint64_t key_meta, const void *target,
                       void *property_obj, uint64_t property_meta, int32_t has_property) {
  TzMetaEntry *e = find(key, key_obj, key_meta, target, property_obj, property_meta, has_property);
  if (!e) {
    return 0;
  }
  release_entry(e);
  uint32_t at = (uint32_t)(e - tz_meta_store.entries);
  memmove(e, e + 1, (tz_meta_store.count - at - 1) * sizeof(TzMetaEntry));
  --tz_meta_store.count;

  return 1;
}

void tz_meta_shutdown(void) {
  while (tz_meta_store.count > 0) {
    release_entry(&tz_meta_store.entries[--tz_meta_store.count]);
  }
  tz_free(tz_meta_store.entries);
  tz_meta_store.entries = NULL;
  tz_meta_store.capacity = 0;
}
