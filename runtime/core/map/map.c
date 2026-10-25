// Dictionaries: Record, Map and Set (docs/objects.md, section 9), which are
// one hash table (TzMap in tzrt.h) that keeps its keys in the order they
// were added.
//
// Keys compare as JavaScript's Map compares them (SameValueZero): strings
// and numbers by value, NaN equal to NaN and -0 to 0; objects, arrays and
// functions by reference; a Json as `===` compares it. A decimal compares by
// value, as `==` compares decimals (docs/decimal.md): 0.30 and 0.3 are one
// key. Each entry keeps the hash of its key, so growing the table hashes
// nothing again.
//
// Compiled code finds and adds entries through tz_map_find and tz_map_add,
// and through the variants for strings, whose fast path for a short string
// (one that lives in its value) is inlined into the program. It stores the
// values itself, at the offset the layout gives.

#include "runtime/core/tzrt/tzrt.h"
#include <math.h>
#include <string.h>

// String values; must match runtime/core/string/string.c.
#define STR_LEN_MASK ((1ull << 30) - 1)
#define STR_TWO_BYTE (1ull << 61)
#define STR_ROPE (1ull << 62)
#define STR_HEAP (1ull << 63)

static inline int str_is_inline(uint64_t meta) {
  return (meta & (STR_ROPE | STR_HEAP | STR_TWO_BYTE)) == STR_ROPE;
}

static inline char *entry_at(const TzMap *m, uint32_t i) {
  return m->entries + (size_t)i * m->layout->entry_size;
}

static inline TzMapEntry *header(void *entry) { return (TzMapEntry *)entry; }

//===----------------------------------------------------------------------===//
// Hashes
//===----------------------------------------------------------------------===//

#define K0 0x9E3779B97F4A7C15ull

static inline uint64_t mix_word(uint64_t h, uint64_t w) {
  h = (h ^ w) * K0;

  return h ^ (h >> 32);
}

static inline uint32_t finish(uint64_t h) {
  h ^= h >> 29;
  h *= 0xBF58476D1CE4E5B9ull;
  h ^= h >> 32;

  return (uint32_t)h;
}

// The hash of a string, the same whatever holds its code units: they are
// taken eight at a time, each folded to a byte (a unit below 256 is its own
// byte), so a short string, which lives in its value, hashes as the word it
// is, and a two-byte string hashes as a one-byte one with the same text.
// Texts that differ only in the upper bytes of units collide, which costs a
// comparison.
static inline uint32_t inline_hash(void *obj, uint64_t meta) {
  return finish(mix_word(meta & STR_LEN_MASK, (uint64_t)(uintptr_t)obj));
}

static uint32_t units_hash(const void *data, uint32_t len, int two, int fold) {
  uint64_t h = len;
  if (!two && !fold && len >= 8) {
    // One-byte units are their own bytes: eight of them are one load, and
    // the last word, which holds fewer, the last eight bytes shifted down.
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t i = 0;
    uint64_t w;
    for (; i + 8 <= len; i += 8) {
      memcpy(&w, bytes + i, 8);
      h = mix_word(h, w);
    }
    if (i < len) {
      memcpy(&w, bytes + len - 8, 8);
      h = mix_word(h, w >> (8 * (8 - (len - i))));
    }
    return finish(h);
  }
  for (uint32_t i = 0; i < len; i += 8) {
    uint64_t w = 0;
    for (uint32_t k = 0; k < 8 && i + k < len; ++k) {
      uint32_t unit = two ? ((const uint16_t *)data)[i + k] : ((const uint8_t *)data)[i + k];
      if (fold && unit >= 'a' && unit <= 'z') {
        unit -= 'a' - 'A';
      }
      w |= (uint64_t)(uint8_t)(unit ^ (unit >> 8)) << (8 * k);
    }
    h = mix_word(h, w);
  }

  return finish(h);
}

static uint32_t string_hash(TzStr s, int fold) {
  if (str_is_inline(s.meta) && !fold) {
    return inline_hash(s.obj, s.meta);
  }
  uint32_t length;
  int two;
  const void *data = tz_str_units(&s, &length, &two);

  return units_hash(data, length, two, fold);
}

// The text of two strings with the case of ASCII letters ignored, for the
// names of variables on Windows.
static int folded_equal(TzStr a, TzStr b) {
  uint32_t la, lb;
  int ta, tb;
  const void *da = tz_str_units(&a, &la, &ta);
  const void *db = tz_str_units(&b, &lb, &tb);
  if (la != lb) {
    return 0;
  }
  for (uint32_t i = 0; i < la; ++i) {
    uint32_t x = ta ? ((const uint16_t *)da)[i] : ((const uint8_t *)da)[i];
    uint32_t y = tb ? ((const uint16_t *)db)[i] : ((const uint8_t *)db)[i];
    if (x >= 'a' && x <= 'z') {
      x -= 'a' - 'A';
    }
    if (y >= 'a' && y <= 'z') {
      y -= 'a' - 'A';
    }
    if (x != y) {
      return 0;
    }
  }

  return 1;
}

// A double as a key: -0 is 0, and every NaN is one NaN.
static inline uint64_t double_bits(double d) {
  if (d == 0) {
    d = 0;
  }
  uint64_t bits;
  memcpy(&bits, &d, sizeof bits);
  if (isnan(d)) {
    bits = 0x7FF8000000000000ull;
  }

  return bits;
}

static uint32_t value_hash(const TzType *type, const void *key, int fold);

static uint32_t json_hash(const TzJson *j) {
  if (!j) {
    return 0x5bd1e995u;
  }
  switch (j->kind) {
  case TZ_JSON_NULL:
    return 0x27d4eb2du;
  case TZ_JSON_BOOL:
    return finish(mix_word(TZ_JSON_BOOL, (uint64_t)(j->u.boolean != 0)));
  case TZ_JSON_NUMBER:
    return finish(mix_word(TZ_JSON_NUMBER, double_bits(j->u.number.value)));
  case TZ_JSON_STRING:
    return string_hash(j->u.string, 0);
  default: // an array or an object: the one it is
    return finish(mix_word(TZ_JSON_OBJECT, (uint64_t)(uintptr_t)j));
  }
}

static uint32_t value_hash(const TzType *type, const void *key, int fold) {
  switch (type->kind) {
  case TZ_T_INT:
    return finish(mix_word(TZ_T_INT, (uint64_t)(uint32_t)*(const int32_t *)key));
  case TZ_T_LONG:
    return finish(mix_word(TZ_T_LONG, (uint64_t)*(const int64_t *)key));
  case TZ_T_DOUBLE:
    return finish(mix_word(TZ_T_DOUBLE, double_bits(*(const double *)key)));
  case TZ_T_BOOL:
    return finish(mix_word(TZ_T_BOOL, *(const uint8_t *)key != 0));
  case TZ_T_STRING:
    return string_hash(*(const TzStr *)key, fold);
  case TZ_T_DECIMAL: {
    // By value: equal decimals have the same text.
    TzBuf text;
    tz_buf_init(&text);
    tz_buf_decimal(&text, *(const TzDec *)key);
    uint32_t hash = units_hash(text.data, (uint32_t)text.length, 1, 0);
    tz_buf_free(&text);
    return hash;
  }
  case TZ_T_FUNCTION: {
    const TzFunction *fn = (const TzFunction *)key;
    return finish(mix_word((uint64_t)(uintptr_t)fn->info, (uint64_t)(uintptr_t)fn->env));
  }
  case TZ_T_JSON:
    return json_hash(*(TzJson *const *)key);
  case TZ_T_OPTIONAL:
    if (!tz_optional_has(type, key)) {
      return 0x9e3779b9u; // undefined
    }
    return value_hash(type->inner, key, fold);
  case TZ_T_ATOM: // a string literal type: the number of its string
    return finish(mix_word(TZ_T_ATOM, (uint64_t)*(const uint32_t *)key));
  case TZ_T_UNION: { // what the member it holds gives
    if (*(const uint32_t *)key == 0) {
      return 0x9e3779b9u; // undefined
    }
    const void *held = key;
    const TzType *member = tz_resolve(type, &held);
    return value_hash(member, held, fold);
  }
  default: // an array, an object or a dictionary: the one it is
    return finish(mix_word(TZ_T_OBJECT, (uint64_t)(uintptr_t)*(void *const *)key));
  }
}

// SameValueZero, with strings compared without case for `fold`.
static int values_equal(const TzType *type, const void *a, const void *b, int fold) {
  switch (type->kind) {
  case TZ_T_INT:
    return *(const int32_t *)a == *(const int32_t *)b;
  case TZ_T_LONG:
    return *(const int64_t *)a == *(const int64_t *)b;
  case TZ_T_DOUBLE: {
    double x = *(const double *)a, y = *(const double *)b;
    return x == y || (isnan(x) && isnan(y));
  }
  case TZ_T_BOOL:
    return (*(const uint8_t *)a != 0) == (*(const uint8_t *)b != 0);
  case TZ_T_STRING: {
    const TzStr *x = (const TzStr *)a, *y = (const TzStr *)b;
    if (fold) {
      return folded_equal(*x, *y);
    }
    return tz_str_equal(x->obj, x->meta, y->obj, y->meta);
  }
  case TZ_T_DECIMAL: {
    const TzDec *x = (const TzDec *)a, *y = (const TzDec *)b;
    return tz_dec_equal(x->coef, x->meta, y->coef, y->meta);
  }
  case TZ_T_FUNCTION: {
    const TzFunction *x = (const TzFunction *)a, *y = (const TzFunction *)b;
    return x->info == y->info && x->env == y->env;
  }
  case TZ_T_JSON: {
    const TzJson *x = *(TzJson *const *)a, *y = *(TzJson *const *)b;
    // As Map compares them: one NaN equals another (JSON has none, though).
    return tz_json_strict_equal(x, y);
  }
  case TZ_T_OPTIONAL: {
    int has_a = tz_optional_has(type, a), has_b = tz_optional_has(type, b);
    if (!has_a || !has_b) {
      return has_a == has_b;
    }
    return values_equal(type->inner, a, b, fold);
  }
  case TZ_T_ATOM:
    return *(const int32_t *)a == *(const int32_t *)b;
  case TZ_T_UNION: { // of the same member, and equal there
    uint32_t tag = *(const uint32_t *)a;
    if (tag != *(const uint32_t *)b) {
      return 0;
    }
    if (tag == 0) { // both undefined
      return 1;
    }
    const void *x = a, *y = b;
    tz_resolve(type, &y);
    const TzType *member = tz_resolve(type, &x);
    return values_equal(member, x, y, fold);
  }
  default:
    return *(void *const *)a == *(void *const *)b;
  }
}

//===----------------------------------------------------------------------===//
// The table
//===----------------------------------------------------------------------===//

TzMap *tz_map_new(const TzMapLayout *layout) {
  // The maps of a type that may form cycles are members of groups.
  int member = (layout->base.flags & TZ_LAYOUT_GROUP) != 0;
  TzMap *m = (TzMap *)(member ? tz_group_alloc(sizeof(TzMap)) : tz_obj_alloc(sizeof(TzMap)));
  int64_t rc = member ? m->rc : 1;
  memset(m, 0, sizeof *m);
  m->rc = rc;
  m->layout = layout;

  return m;
}

static void index_insert(TzMap *m, uint32_t hash, uint32_t position) {
  uint32_t slot = hash & m->index_mask;
  while (m->index[slot]) {
    slot = (slot + 1) & m->index_mask;
  }
  m->index[slot] = position + 1;
}

// The index for the entries as they lie, sized for `capacity` of them: at
// least twice as many slots, so that probes stay short.
static void rebuild_index(TzMap *m) {
  tz_free(m->index);
  m->index = NULL;
  m->index_mask = 0;
  if (m->capacity <= TZ_MAP_SMALL) {
    return;
  }
  uint32_t size = 16;
  while (size < 2 * m->capacity) {
    size *= 2;
  }
  m->index = (uint32_t *)tz_alloc(size * sizeof(uint32_t));
  memset(m->index, 0, size * sizeof(uint32_t));
  m->index_mask = size - 1;
  for (uint32_t i = 0; i < m->used; ++i) {
    TzMapEntry *e = header(entry_at(m, i));
    if (e->live) {
      index_insert(m, e->hash, i);
    }
  }
}

// Moves the live entries over the holes, keeping their order; only while
// nothing iterates.
static void compact(TzMap *m) {
  uint32_t size = m->layout->entry_size, to = 0;
  for (uint32_t i = 0; i < m->used; ++i) {
    char *e = entry_at(m, i);
    if (!header(e)->live) {
      continue;
    }
    if (to != i) {
      memcpy(entry_at(m, to), e, size);
    }
    ++to;
  }
  m->used = to;
  rebuild_index(m);
}

// Room for one more entry: the holes, if at least half the entries are
// deleted and nothing iterates; otherwise twice the room.
TZ_NOINLINE static void make_room(TzMap *m) {
  if (m->iterating == 0 && m->count <= m->used / 2 && m->used >= TZ_MAP_SMALL) {
    compact(m);
    return;
  }
  uint32_t size = m->layout->entry_size;
  uint32_t capacity = m->capacity ? m->capacity * 2 : 4;
  if (capacity > (1u << 30)) {
    tz_panicf("a dictionary cannot hold more than %u entries", 1u << 30);
  }
  char *entries = (char *)tz_alloc((size_t)capacity * size);
  if (m->used) {
    memcpy(entries, m->entries, (size_t)m->used * size);
  }
  tz_free(m->entries);
  m->entries = entries;
  m->capacity = capacity;
  if (m->iterating == 0 && m->count < m->used) {
    compact(m); // rebuilds the index
  } else {
    rebuild_index(m);
  }
}

static void *find_hashed(const TzMap *m, const void *key, uint32_t hash) {
  const TzMapLayout *l = m->layout;
  int fold = (m->flags & TZ_MAP_FOLD) != 0;
  if (!m->index) {
    for (uint32_t i = 0; i < m->used; ++i) {
      char *e = entry_at(m, i);
      if (header(e)->hash == hash && header(e)->live &&
          values_equal(l->key, e + l->key_offset, key, fold)) {
        return e;
      }
    }
    return NULL;
  }
  for (uint32_t slot = hash & m->index_mask;; slot = (slot + 1) & m->index_mask) {
    uint32_t position = m->index[slot];
    if (!position) {
      return NULL;
    }
    char *e = entry_at(m, position - 1);
    if (header(e)->hash == hash && header(e)->live &&
        values_equal(l->key, e + l->key_offset, key, fold)) {
      return e;
    }
  }
}

static inline uint32_t key_hash(const TzMap *m, const void *key) {
  return value_hash(m->layout->key, key, (m->flags & TZ_MAP_FOLD) != 0);
}

// A new entry for a key the map lacks: a copy of the key, retained (a string
// in its flat form, which later comparisons read directly), and a value of
// zero.
static void *append(TzMap *m, const void *key, uint32_t hash) {
  if (m->used == m->capacity) {
    make_room(m);
  }
  const TzMapLayout *l = m->layout;
  uint32_t position = m->used++;
  char *e = entry_at(m, position);
  memset(e, 0, l->entry_size);
  header(e)->hash = hash;
  header(e)->live = 1;
  char *k = e + l->key_offset;
  memcpy(k, key, l->key->size);
  if (l->key->kind == TZ_T_STRING) {
    TzStr *s = (TzStr *)k;
    uint32_t length;
    int two;
    tz_str_units(s, &length, &two);
  } else if (l->key->kind == TZ_T_DOUBLE) {
    double *d = (double *)k;
    if (*d == 0) {
      *d = 0; // -0 becomes 0, as JavaScript's Map makes it
    }
  }
  tz_value_retain(l->key, k);
  if (m->rc < 0) {
    tz_group_store_value(m, l->key, k);
    tz_store_barrier(m, l->key, k);
  }
  if (m->index) {
    index_insert(m, hash, position);
  }
  ++m->count;

  return e;
}

void *tz_map_find(TzMap *m, const void *key) {
  if (m->count == 0) {
    return NULL;
  }

  return find_hashed(m, key, key_hash(m, key));
}

void *tz_map_add(TzMap *m, const void *key, int32_t *added) {
  uint32_t hash = key_hash(m, key);
  void *e = m->count ? find_hashed(m, key, hash) : NULL;
  *added = e == NULL;

  return e ? e : append(m, key, hash);
}

// Strings, the usual keys. A short one is its own word, so its hash is a
// multiplication and a match a comparison of words; anything else takes the
// way above.
//
// Whether an entry holds the short string key `obj, meta` of hash `hash`. A
// stored key that is equal but not short is a two-byte string.
TZ_NOINLINE int32_t tz_map_str_key_equal(const TzStr *stored, void *obj, uint64_t meta) {
  return tz_str_equal(stored->obj, stored->meta, obj, meta);
}

static inline int short_key_matches(const char *e, uint32_t offset, uint32_t hash, void *obj,
                                    uint64_t meta) {
  const TzStr *stored = (const TzStr *)(e + offset);
  if (header((void *)e)->hash != hash || !header((void *)e)->live) {
    return 0;
  }
  if (stored->obj == obj && stored->meta == meta) {
    return 1;
  }

  return !str_is_inline(stored->meta) && tz_map_str_key_equal(stored, obj, meta);
}

// A map without an index: its entries one by one.
TZ_NOINLINE void *tz_map_find_short_small(TzMap *m, void *obj, uint64_t meta, uint32_t hash) {
  uint32_t size = m->layout->entry_size, offset = m->layout->key_offset;
  for (uint32_t i = 0; i < m->used; ++i) {
    char *e = m->entries + (size_t)i * size;
    if (short_key_matches(e, offset, hash, obj, meta)) {
      return e;
    }
  }

  return NULL;
}

TZ_NOINLINE void *tz_map_find_str_slow(TzMap *m, void *obj, uint64_t meta) {
  if (str_is_inline(meta) && !(m->flags & TZ_MAP_FOLD) && !m->index) {
    return tz_map_find_short_small(m, obj, meta, inline_hash(obj, meta));
  }
  TzStr key = {obj, meta};

  return tz_map_find(m, &key);
}

// A map with an index, from slot `slot` on.
TZ_NOINLINE void *tz_map_find_short_probe(TzMap *m, void *obj, uint64_t meta, uint32_t hash,
                                          uint32_t slot) {
  uint32_t size = m->layout->entry_size, offset = m->layout->key_offset;
  for (;; slot = (slot + 1) & m->index_mask) {
    uint32_t position = m->index[slot];
    if (!position) {
      return NULL;
    }
    char *e = m->entries + (size_t)(position - 1) * size;
    if (short_key_matches(e, offset, hash, obj, meta)) {
      return e;
    }
  }
}

// The first entry the index points at is usually the one: that is inlined
// into the program, and the rest is called.
TZ_HOT void *tz_map_find_str(TzMap *m, void *obj, uint64_t meta) {
  if (!str_is_inline(meta) || !m->index || (m->flags & TZ_MAP_FOLD)) {
    return tz_map_find_str_slow(m, obj, meta);
  }
  uint32_t hash = inline_hash(obj, meta);
  uint32_t slot = hash & m->index_mask;
  uint32_t position = m->index[slot];
  if (!position) {
    return NULL;
  }
  char *e = m->entries + (size_t)(position - 1) * m->layout->entry_size;
  const TzStr *key = (const TzStr *)(e + m->layout->key_offset);
  if (header(e)->hash == hash && key->obj == obj && key->meta == meta && header(e)->live) {
    return e;
  }

  return tz_map_find_short_probe(m, obj, meta, hash, slot);
}

TZ_NOINLINE void *tz_map_append_str(TzMap *m, void *obj, uint64_t meta) {
  TzStr key = {obj, meta};

  return append(m, &key, key_hash(m, &key));
}

TZ_HOT void *tz_map_add_str(TzMap *m, void *obj, uint64_t meta, int32_t *added) {
  void *e = tz_map_find_str(m, obj, meta);
  *added = e == NULL;

  return e ? e : tz_map_append_str(m, obj, meta);
}

void *tz_map_next(const TzMap *m, uint32_t *at) {
  while (*at < m->used) {
    char *e = entry_at(m, (*at)++);
    if (header(e)->live) {
      return e;
    }
  }

  return NULL;
}

// The key and the value of a live entry become zero, and are released after
// it is marked deleted.
static void remove_entry(TzMap *m, char *e) {
  const TzMapLayout *l = m->layout;
  uint32_t key_size = l->key->size, value_size = l->value ? l->value->size : 0;
  char small[128];
  char *key =
      key_size + value_size <= sizeof small ? small : (char *)tz_alloc(key_size + value_size);
  char *value = key + key_size;
  memcpy(key, e + l->key_offset, key_size);
  memset(e + l->key_offset, 0, key_size);
  if (value_size) {
    memcpy(value, e + l->value_offset, value_size);
    memset(e + l->value_offset, 0, value_size);
  }
  header(e)->live = 0;
  --m->count;
  if (m->count == 0 && m->iterating == 0) {
    // Empty: the entries are free again.
    m->used = 0;
    if (m->index) {
      memset(m->index, 0, ((size_t)m->index_mask + 1) * sizeof(uint32_t));
    }
  }
  tz_value_release_in(m, l->key, key);
  if (value_size) {
    tz_value_release_in(m, l->value, value);
  }
  if (key != small) {
    tz_free(key);
  }
}

int32_t tz_map_delete(TzMap *m, const void *key) {
  char *e = (char *)tz_map_find(m, key);
  if (!e) {
    return 0;
  }
  if (m->flags & TZ_MAP_ENV) {
    tz_process_env_changed((const TzStr *)(e + m->layout->key_offset), NULL);
  }
  remove_entry(m, e);

  return 1;
}

TZ_NOINLINE int32_t tz_map_delete_str(TzMap *m, void *obj, uint64_t meta) {
  TzStr key = {obj, meta};

  return tz_map_delete(m, &key);
}

void tz_map_clear(TzMap *m) {
  for (uint32_t i = 0; i < m->used; ++i) {
    char *e = entry_at(m, i);
    if (header(e)->live) {
      remove_entry(m, e);
    }
  }
}

// After compiled code stored the value of a Record's entry: process.env
// tells the environment.
void tz_map_stored(TzMap *m, void *entry) {
  if (m->flags & TZ_MAP_ENV) {
    tz_process_env_changed((const TzStr *)((char *)entry + m->layout->key_offset),
                           (const TzStr *)((char *)entry + m->layout->value_offset));
  }
}

// Maps whose count reached zero while another one was being freed: freed one
// after another, so that a chain of maps (a trie, a map of maps of maps)
// takes no stack however long it is. Not static: see the rules in tzrt.h.
TZ_THREAD_LOCAL TzMap *tz_map_dead; // each thread frees its own
TZ_THREAD_LOCAL int32_t tz_map_dropping;

// What a map holds: its entries, and what they hold.
static void release_entries(TzMap *m) {
  const TzMapLayout *l = m->layout;
  int managed_key = tz_is_managed(l->key);
  int managed_value = l->value && tz_is_managed(l->value);
  if (managed_key || managed_value) {
    for (uint32_t i = 0; i < m->used; ++i) {
      char *e = entry_at(m, i);
      if (!header(e)->live) {
        continue;
      }
      if (managed_key) {
        tz_value_release_in(m, l->key, e + l->key_offset);
      }
      if (managed_value) {
        tz_value_release_in(m, l->value, e + l->value_offset);
      }
    }
  }
  tz_free(m->entries);
  tz_free(m->index);
}

#if TZ_PARALLEL
// publish.c: what a map other threads may reach holds, which is shared now.
void tz_map_publish(TzMap *m) {
  const TzMapLayout *l = m->layout;
  int managed_key = tz_is_managed(l->key);
  int managed_value = l->value && tz_is_managed(l->value);
  if (!managed_key && !managed_value) {
    return;
  }
  for (uint32_t i = 0; i < m->used; ++i) {
    char *e = entry_at(m, i);
    if (!header(e)->live) {
      continue;
    }
    if (managed_key) {
      tz_publish_value(l->key, e + l->key_offset);
    }
    if (managed_value) {
      tz_publish_value(l->value, e + l->value_offset);
    }
  }
}
#endif

void tz_map_drop(void *object) {
  TzMap *m = (TzMap *)object;
  // A member of a group, which frees its memory with the others: what it
  // holds goes now, while the other members are still there.
  if (m->rc < 0) {
    release_entries(m);
    m->entries = NULL;
    m->index = NULL;
    m->used = m->count = m->capacity = 0;
    return;
  }
  if (tz_map_dropping) {
    m->next_dead = tz_map_dead;
    tz_map_dead = m;
    return;
  }
  tz_map_dropping = 1;
  while (m) {
    release_entries(m);
    if (tz_group_debugging) {
      tz_group_forget_unstored(m);
    }
    tz_obj_free(m);
    m = tz_map_dead;
    if (m) {
      tz_map_dead = m->next_dead;
    }
  }
  tz_map_dropping = 0;
}

//===----------------------------------------------------------------------===//
// Whole maps
//===----------------------------------------------------------------------===//

// `{ ...r }` and new Map(m): the same keys and values in a new map.
TzMap *tz_map_copy(const TzMap *m, const TzMapLayout *layout) {
  TzMap *copy = tz_map_new(layout);
  uint32_t at = 0;
  for (char *e; (e = (char *)tz_map_next(m, &at));) {
    // The hashes of keys that ignore case are not those of the copy.
    const void *key = e + m->layout->key_offset;
    uint32_t hash = m->flags & TZ_MAP_FOLD ? key_hash(copy, key) : header(e)->hash;
    char *to = (char *)append(copy, key, hash);
    if (layout->value) {
      memcpy(to + layout->value_offset, e + m->layout->value_offset, layout->value->size);
      tz_value_retain(layout->value, to + layout->value_offset);
      if (copy->rc < 0) {
        tz_group_store_value(copy, layout->value, to + layout->value_offset);
      }
    }
  }

  return copy;
}

// Object.keys of a Record, [...m.keys()] and the like: the keys (or the
// values) as an array of `element`.
TzArray *tz_map_keys(const TzMap *m, const TzType *element) {
  TzArray *a = tz_arr_new(element, 0, m->count);
  uint32_t at = 0;
  for (char *e; (e = (char *)tz_map_next(m, &at));) {
    tz_value_retain(element, e + m->layout->key_offset);
    tz_arr_push_value(a, e + m->layout->key_offset);
  }

  return a;
}

TzArray *tz_map_values(const TzMap *m, const TzType *element) {
  TzArray *a = tz_arr_new(element, 0, m->count);
  uint32_t at = 0;
  for (char *e; (e = (char *)tz_map_next(m, &at));) {
    tz_value_retain(element, e + m->layout->value_offset);
    tz_arr_push_value(a, e + m->layout->value_offset);
  }

  return a;
}

// Object.entries of a Record: a new [key, value] for each entry, laid out
// as `tuple` says.
TzArray *tz_map_entries(const TzMap *m, const TzType *element, const TzLayout *tuple,
                        int64_t size) {
  TzArray *a = tz_arr_new(element, 0, m->count);
  const TzMapLayout *l = m->layout;
  uint32_t at = 0;
  for (char *e; (e = (char *)tz_map_next(m, &at));) {
    TzInstance *pair = (TzInstance *)tz_obj_alloc(size);
    memset(pair, 0, (size_t)size);
    pair->rc = 1;
    pair->layout = tuple;
    char *key = (char *)pair + tuple->fields[0].offset;
    char *value = (char *)pair + tuple->fields[1].offset;
    memcpy(key, e + l->key_offset, l->key->size);
    tz_value_retain(l->key, key);
    memcpy(value, e + l->value_offset, l->value->size);
    tz_value_retain(l->value, value);
    tz_arr_push_value(a, &pair);
  }

  return a;
}

// The methods of ES2025 that make a set from two: union (0), intersection
// (1), difference (2) and symmetricDifference (3). `b` is a Set or a Map,
// whose keys are its elements; the order is that of the spec.
TzMap *tz_map_set_op(const TzMap *a, const TzMap *b, int32_t op) {
  const TzMapLayout *l = a->layout;
  TzMap *result;
  int32_t added;
  uint32_t at = 0;
  char *e;
  switch (op) {
  case 0:
    result = tz_map_copy(a, l);
    while ((e = (char *)tz_map_next(b, &at))) {
      tz_map_add(result, e + b->layout->key_offset, &added);
    }
    return result;
  case 1: {
    result = tz_map_new(l);
    // The smaller one is walked, and gives the order.
    const TzMap *walked = a->count <= b->count ? a : b, *other = walked == a ? b : a;
    while ((e = (char *)tz_map_next(walked, &at))) {
      if (tz_map_find((TzMap *)other, e + walked->layout->key_offset)) {
        tz_map_add(result, e + walked->layout->key_offset, &added);
      }
    }
    return result;
  }
  case 2:
    result = tz_map_new(l);
    while ((e = (char *)tz_map_next(a, &at))) {
      if (!tz_map_find((TzMap *)b, e + l->key_offset)) {
        tz_map_add(result, e + l->key_offset, &added);
      }
    }
    return result;
  default:
    result = tz_map_copy(a, l);
    while ((e = (char *)tz_map_next(b, &at))) {
      const void *key = e + b->layout->key_offset;
      if (tz_map_find((TzMap *)a, key)) {
        tz_map_delete(result, key);
      } else {
        tz_map_add(result, key, &added);
      }
    }
    return result;
  }
}

// isSubsetOf (0), isSupersetOf (1) and isDisjointFrom (2).
int32_t tz_map_set_test(const TzMap *a, const TzMap *b, int32_t op) {
  uint32_t at = 0;
  char *e;
  if (op == 0) {
    if (a->count > b->count) {
      return 0;
    }
    while ((e = (char *)tz_map_next(a, &at))) {
      if (!tz_map_find((TzMap *)b, e + a->layout->key_offset)) {
        return 0;
      }
    }
    return 1;
  }
  if (op == 1) {
    if (a->count < b->count) {
      return 0;
    }
    while ((e = (char *)tz_map_next(b, &at))) {
      if (!tz_map_find((TzMap *)a, e + b->layout->key_offset)) {
        return 0;
      }
    }
    return 1;
  }
  const TzMap *walked = a->count <= b->count ? a : b, *other = walked == a ? b : a;
  while ((e = (char *)tz_map_next(walked, &at))) {
    if (tz_map_find((TzMap *)other, e + walked->layout->key_offset)) {
      return 0;
    }
  }

  return 1;
}
