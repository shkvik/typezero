// Dictionaries: Record, Map and Set.

#ifndef TZ_MAP_H
#define TZ_MAP_H

#include "runtime/core/tzrt/tzrt.h"

// A dictionary of the language: a Record, a Map or a Set (docs/objects.md,
// section 9). All three are one hash table that keeps its entries in an
// array in the order they were added, as V8's ordered hash tables and the
// dicts of CPython do: iterating walks the array, a deleted entry stays
// there as a hole until the array is compacted, and an index of positions,
// an open-addressing table, finds a key by its hash. A map of up to
// TZ_MAP_SMALL entries has no index; it compares the hashes of its entries
// one by one.
//
// An entry is TzMapEntry followed by the key and the value (none for a Set)
// at the offsets of the map's layout, which the compiler lays out; compiled
// code reads and writes entries, `count`, `used` and `iterating` directly.
// While an iteration is in progress (`iterating` > 0) entries do not move,
// so a position stays valid, and keys added meanwhile come after it, as in
// JavaScript. Must match Lowering (compiler/lower/codegen/codegen_dict.cpp).
//
// A map starts as every object of the language does, with its count and its
// layout, whose drop function is tz_map_drop and whose flag TZ_LAYOUT_DICT
// tells printing and JSON.stringify what it is.
enum { TZ_MAP_RECORD = 0, TZ_MAP_MAP = 1, TZ_MAP_SET = 2 };

typedef struct TzMapLayout {
  TzLayout base;
  const TzType *key;
  const TzType *value; // NULL for a Set
  uint32_t kind;       // TZ_MAP_RECORD...
  uint32_t entry_size, key_offset, value_offset;
} TzMapLayout;

#define TZ_MAP_SMALL 8

enum {
  TZ_MAP_ENV = 1,  // process.env: changes go to the environment of the process
  TZ_MAP_FOLD = 2, // the keys ignore case, as names of variables on Windows do
  TZ_MAP_BARE = 4, // a Record of Object.groupBy, an object without a prototype in JavaScript
};

typedef struct TzMap {
  int64_t rc;
  const TzMapLayout *layout;
  char *entries;   // room for `capacity`, `used` of them in use
  uint32_t *index; // positions + 1 by hash (0 for none), or NULL
  uint32_t count;  // live entries: the size
  uint32_t used;   // live and deleted entries, in the order added
  uint32_t capacity;
  uint32_t index_mask;     // the index has index_mask + 1 slots
  uint32_t iterating;      // iterations in progress
  uint32_t flags;          // TZ_MAP_ENV...
  struct TzMap *next_dead; // see tz_map_drop
} TzMap;

typedef struct {
  uint32_t hash;
  uint32_t live; // 0 for a deleted entry
} TzMapEntry;

TzMap *tz_map_new(const TzMapLayout *layout);
void tz_map_drop(void *map);
// The entry of `key` (borrowed), or NULL.
void *tz_map_find(TzMap *m, const void *key);
// The entry of `key`; if the map lacks it, a new one with a copy of the key
// (retained) and a value of zero, and `*added` is set.
void *tz_map_add(TzMap *m, const void *key, int32_t *added);
int32_t tz_map_delete(TzMap *m, const void *key);
// The next live entry from position `*at` on, or NULL; `*at` moves past it.
void *tz_map_next(const TzMap *m, uint32_t *at);

#endif
