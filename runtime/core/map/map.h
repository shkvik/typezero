#ifndef TZ_MAP_H
#define TZ_MAP_H

#include "runtime/core/tzrt/tzrt.h"

enum { TZ_MAP_RECORD = 0, TZ_MAP_MAP = 1, TZ_MAP_SET = 2 };

typedef struct TzMapLayout {
  TzLayout base;
  const TzType *key;
  const TzType *value;
  uint32_t kind;
  uint32_t entry_size, key_offset, value_offset;
} TzMapLayout;

#define TZ_MAP_SMALL 8

enum {
  TZ_MAP_ENV = 1,
  TZ_MAP_FOLD = 2,
  TZ_MAP_BARE = 4,
};

typedef struct TzMap {
  int64_t rc;
  const TzMapLayout *layout;
  char *entries;
  uint32_t *index;
  uint32_t count;
  uint32_t used;
  uint32_t capacity;
  uint32_t index_mask;
  uint32_t iterating;
  uint32_t flags;
  struct TzMap *next_dead;
} TzMap;

typedef struct {
  uint32_t hash;
  uint32_t live;
} TzMapEntry;

TzMap *tz_map_new(const TzMapLayout *layout);
void tz_map_drop(void *map);
void *tz_map_find(TzMap *m, const void *key);
void *tz_map_add(TzMap *m, const void *key, int32_t *added);
int32_t tz_map_delete(TzMap *m, const void *key);
void *tz_map_next(const TzMap *m, uint32_t *at);

#endif
