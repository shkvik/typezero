#include "runtime/library/intl/intl/intl.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/array/array.h"
#include "runtime/core/closure/closure.h"
#include "runtime/core/error/error.h"
#include "runtime/core/group/group.h"
#include "runtime/core/output/output.h"
#include "runtime/core/parallel/parallel.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *const option_names[TZ_IO_COUNT] = {
#define TZ_INTL_OPTION_NAME(id, name) name,
    TZ_INTL_OPTIONS(TZ_INTL_OPTION_NAME)
#undef TZ_INTL_OPTION_NAME
};

TZ_COLD TZ_NORETURN void tz_intl_no_icu(void) {
  tz_panicf("Intl needs the ICU of the system, which Windows has from Windows 10 1903 on");
}

const TzIcu *tz_intl_icu(void) {
  const TzIcu *icu = tz_icu();
  if (!icu) {
    tz_intl_no_icu();
  }
  if (tz_icu_missing) {
    tz_panicf("Intl needs a newer ICU of the system than this Windows has (ICU 67, from Windows "
              "10 2004 on)");
  }

  return icu;
}

int tz_intl_ascii(TzStr s, char *out, size_t size) {
  uint32_t n;
  int two;
  const void *units = tz_str_units(&s, &n, &two);
  if ((size_t)n + 1 > size) {
    return 0;
  }
  for (uint32_t i = 0; i < n; ++i) {
    unsigned unit = two ? ((const uint16_t *)units)[i] : ((const uint8_t *)units)[i];
    if (unit == 0 || unit >= 0x80) {
      return 0;
    }
    out[i] = (char)unit;
  }
  out[n] = 0;

  return 1;
}

void tz_intl_copy(char *out, size_t size, const char *text) {
  size_t n = strlen(text);
  if (n >= size) {
    n = size - 1;
  }
  memcpy(out, text, n);
  out[n] = 0;
}

const TzUChar *tz_intl_utf16(TzStr *s, TzUChar *buffer, int32_t capacity, TzUChar **heap,
                             int32_t *length) {
  uint32_t n;
  int two;
  const void *units = tz_str_units(s, &n, &two);
  *heap = NULL;
  *length = (int32_t)n;
  if (two) {
    return (const TzUChar *)units;
  }
  TzUChar *out = buffer;
  if ((int64_t)n > capacity) {
    out = *heap = (TzUChar *)tz_alloc((size_t)n * sizeof(TzUChar));
  }
  for (uint32_t i = 0; i < n; ++i) {
    out[i] = ((const uint8_t *)units)[i];
  }

  return out;
}

static int is_listed(const TzIcu *icu, const char *name) {
  int32_t count = icu->uloc_countAvailable();
  for (int32_t i = 0; i < count; ++i) {
    if (strcmp(icu->uloc_getAvailable(i), name) == 0) {
      return 1;
    }
  }

  return 0;
}

static int is_available(const TzIcu *icu, const char *name) {
  if (is_listed(icu, name)) {
    return 1;
  }
  if (!strchr(name, '_')) {
    return 0;
  }
  char full[64];
  TzUErrorCode status = 0;
  int32_t n = icu->uloc_addLikelySubtags(name, full, (int32_t)sizeof full - 1, &status);
  if (TZ_U_FAILURE(status) || n <= 0) {
    return 0;
  }
  full[n] = 0;

  return strcmp(full, name) != 0 && is_listed(icu, full);
}

static void best_available(const TzIcu *icu, const char *requested, char *out, size_t size) {
  char candidate[64];
  tz_intl_copy(candidate, sizeof candidate, requested);
  while (candidate[0]) {
    if (is_available(icu, candidate)) {
      tz_intl_copy(out, size, candidate);
      return;
    }
    char *cut = strrchr(candidate, '_');
    if (!cut) {
      break;
    }
    *cut = 0;
  }
  out[0] = 0;
}

static int parse_tag(const TzIcu *icu, TzStr locale, char *id, int32_t size) {
  char tag[128];
  if (!tz_intl_ascii(locale, tag, sizeof tag) || !tag[0]) {
    return 0;
  }
  TzUErrorCode status = 0;
  int32_t parsed = 0;
  int32_t n = icu->uloc_forLanguageTag(tag, id, size - 1, &parsed, &status);
  if (TZ_U_FAILURE(status) || n < 0 || parsed != (int32_t)strlen(tag) ||
      strncmp(tag, "x-", 2) == 0 || strcmp(tag, "root") == 0) {
    return 0;
  }
  id[n] = 0;

  return 1;
}

static void canonical_id(const TzIcu *icu, const char *id, char *out, int32_t size) {
  TzUErrorCode status = 0;
  int32_t n = icu->uloc_canonicalize(id, out, size - 1, &status);
  if (TZ_U_FAILURE(status) || n <= 0) {
    tz_intl_copy(out, (size_t)size, id);
  } else {
    out[n] = 0;
  }
}

int tz_intl_resolve_locale(const TzIcu *icu, int has_locale, TzStr locale, char *requested,
                           char *found) {
  if (has_locale) {
    char id[160];
    if (!parse_tag(icu, locale, id, sizeof id)) {
      tz_panicf("internal error: a language tag that is not one");
    }
    canonical_id(icu, id, requested, 160);
  } else {
    tz_intl_copy(requested, 160, tz_icu_default_locale());
  }
  char base[64];
  TzUErrorCode status = 0;
  int32_t n = icu->uloc_getBaseName(requested, base, (int32_t)sizeof base - 1, &status);
  base[TZ_U_FAILURE(status) || n < 0 ? 0 : n] = 0;
  best_available(icu, base, found, 64);
  if (!found[0]) {
    best_available(icu, tz_icu_default_locale(), found, 64);
  }
  if (!found[0]) {
    tz_intl_copy(found, 64, "en_US");
  }

  return 1;
}

static const char *legacy_key(const char *bcp_key) {
  static const char *const keys[][2] = {{"nu", "numbers"},    {"co", "collation"},
                                        {"kn", "colnumeric"}, {"kf", "colcasefirst"},
                                        {"ca", "calendar"},   {"hc", "hours"}};
  for (size_t i = 0; i < sizeof keys / sizeof keys[0]; ++i) {
    if (strcmp(keys[i][0], bcp_key) == 0) {
      return keys[i][1];
    }
  }

  return bcp_key;
}

void tz_intl_keyword(const TzIcu *icu, const char *requested, const char *bcp_key, char *out,
                     size_t size) {
  char value[64];
  TzUErrorCode status = 0;
  int32_t n = icu->uloc_getKeywordValue(requested, legacy_key(bcp_key), value,
                                        (int32_t)sizeof value - 1, &status);
  out[0] = 0;
  if (TZ_U_FAILURE(status) || n <= 0) {
    return;
  }
  value[n] = 0;
  const char *bcp = icu->uloc_toUnicodeLocaleType(bcp_key, value);
  tz_intl_copy(out, size, bcp ? bcp : value);
}

void tz_intl_tag(const TzIcu *icu, const char *id, char *out, size_t size) {
  TzUErrorCode status = 0;
  int32_t n = icu->uloc_toLanguageTag(id, out, (int32_t)size - 1, 0, &status);
  out[TZ_U_FAILURE(status) || n < 0 ? 0 : n] = 0;
}

int tz_intl_is_keyword_type(const char *text) {
  size_t run = 0;
  for (const char *p = text;; ++p) {
    if (*p == '-' || *p == 0) {
      if (run < 3 || run > 8) {
        return 0;
      }
      if (*p == 0) {
        return 1;
      }
      run = 0;
    } else if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')) {
      ++run;
    } else {
      return 0;
    }
  }
}

int tz_intl_has_numbering(const TzIcu *icu, const char *name) {
  TzUErrorCode status = 0;
  void *system = icu->unumsys_openByName(name, &status);
  if (TZ_U_FAILURE(status) || !system) {
    return 0;
  }
  int ok = !icu->unumsys_isAlgorithmic(system);
  icu->unumsys_close(system);

  return ok;
}

const char *tz_intl_option(const TzIntlOptions *o, int index, const char *const *allowed, char *out,
                           const char *method, int *failed) {
  if (*failed || !(o->has_text & (UINT64_C(1) << index))) {
    return NULL;
  }
  int ok = tz_intl_ascii(o->text[index], out, TZ_INTL_TEXT);
  if (!ok) {
    out[0] = 0;
  }
  if (allowed) {
    int found = 0;
    for (int i = 0; ok && allowed[i] && !found; ++i) {
      found = strcmp(allowed[i], out) == 0;
    }
    if (!found) {
      tz_panicf("internal error: the value %s of the option %s of %s", ok ? out : "?",
                option_names[index], method);
    }
  }

  return out;
}

int32_t tz_intl_number_option(const TzIntlOptions *o, int index, int32_t min, int32_t max,
                              int32_t fallback, int *failed) {
  if (*failed || !(o->has_number & (UINT64_C(1) << index))) {
    return fallback;
  }
  double v = o->number[index];
  if (isnan(v)) {
    return fallback;
  }
  v = v < min ? min : v > max ? max : v;

  return (int32_t)floor(v);
}

int tz_intl_bool_option(const TzIntlOptions *o, int index) {
  if (!(o->has_number & (UINT64_C(1) << index))) {
    return -1;
  }

  return o->number[index] != 0;
}

TzIntl *tz_intl_alloc(size_t size, int32_t kind, const TzLayout *layout) {
  TzIntl *o = (TzIntl *)tz_obj_alloc((int64_t)size);
  memset(o, 0, size);
  o->rc = 1;
  o->layout = layout;
  o->kind = kind;
  for (int i = 0; i < TZ_IO_COUNT; ++i) {
    o->number[i] = NAN;
  }

  return o;
}

void tz_intl_set_text(TzIntl *o, int index, const char *text) {
  tz_intl_copy(o->text[index], TZ_INTL_TEXT, text);
}

void *tz_intl_new(int32_t kind, const TzIntlOptions *o) {
  switch (kind) {
  case TZ_INTL_NUMBER_FORMAT:
    return tz_nf_create(o, "Intl.NumberFormat");
  case TZ_INTL_COLLATOR:
    return tz_collator_create(o, "Intl.Collator");
  case TZ_INTL_PLURAL_RULES:
    return tz_pr_create(o);
  case TZ_INTL_RELATIVE_TIME_FORMAT:
    return tz_rtf_create(o);
  case TZ_INTL_LIST_FORMAT:
    return tz_lf_create(o);
  case TZ_INTL_DISPLAY_NAMES:
    return tz_dn_create(o);
  default:
    return tz_seg_create(o);
  }
}

void tz_intl_drop(void *object) { ((TzInstance *)object)->layout->drop(object); }

static void release(TzIntl *o) {
  if (tz_rc_dec(&o->rc)) {
    o->layout->drop(o);
  }
}

#define CACHED 4
typedef struct {
  uint16_t *key;
  size_t length;
  TzIntl *object;
} CacheEntry;
typedef struct {
  CacheEntry entries[CACHED];
  uint32_t next;
} Cache;

static Cache *cache_of(TzThread *t) {
  Cache *c = (Cache *)t->caches[TZ_CACHE_INTL];
  if (!c) {
    c = (Cache *)calloc(1, sizeof *c);
    if (!c) {
      tz_panic("out of memory", 13);
    }
    t->caches[TZ_CACHE_INTL] = c;
  }

  return c;
}

typedef struct {
  uint16_t local[128];
  uint16_t *units;
  size_t length, capacity;
} Key;

static void key_put(Key *k, uint16_t unit) {
  if (k->length == k->capacity) {
    size_t capacity = k->capacity * 2;
    uint16_t *grown = (uint16_t *)tz_alloc(capacity * sizeof(uint16_t));
    memcpy(grown, k->units, k->length * sizeof(uint16_t));
    if (k->units != k->local) {
      tz_free(k->units);
    }
    k->units = grown;
    k->capacity = capacity;
  }
  k->units[k->length++] = unit;
}

static void key_bits(Key *k, uint64_t bits) {
  for (int i = 0; i < 4; ++i) {
    key_put(k, (uint16_t)(bits >> (16 * i)));
  }
}

static void key_string(Key *k, TzStr s) {
  uint32_t n;
  int two;
  const void *units = tz_str_units(&s, &n, &two);
  key_bits(k, n);
  for (uint32_t i = 0; i < n; ++i) {
    key_put(k, two ? ((const uint16_t *)units)[i] : ((const uint8_t *)units)[i]);
  }
}

static void key_of(Key *k, int32_t kind, const TzIntlOptions *o) {
  k->units = k->local;
  k->length = 0;
  k->capacity = sizeof k->local / sizeof k->local[0];
  key_put(k, (uint16_t)kind);
  key_bits(k, o->has_text);
  key_bits(k, o->has_number);
  key_put(k, (uint16_t)o->has_locale);
  if (o->has_locale) {
    key_string(k, o->locale);
  }
  for (int i = 0; i < TZ_IO_COUNT; ++i) {
    if (o->has_text & (UINT64_C(1) << i)) {
      key_string(k, o->text[i]);
    }
    if (o->has_number & (UINT64_C(1) << i)) {
      uint64_t bits;
      memcpy(&bits, &o->number[i], sizeof bits);
      key_bits(k, bits);
    }
  }
}

static void key_free(Key *k) {
  if (k->units != k->local) {
    tz_free(k->units);
  }
}

TzIntl *tz_intl_cached(int32_t kind, const TzIntlOptions *o, const char *method) {
  Key key;
  key_of(&key, kind, o);
  Cache *cache = cache_of(&tz_thread);
  for (int i = 0; i < CACHED; ++i) {
    CacheEntry *e = &cache->entries[i];
    if (e->object && e->length == key.length &&
        memcmp(e->key, key.units, key.length * sizeof(uint16_t)) == 0) {
      key_free(&key);
      return e->object;
    }
  }
  TzIntl *object =
      kind == TZ_INTL_COLLATOR ? tz_collator_create(o, method) : tz_nf_create(o, method);
  if (!object) {
    key_free(&key);
    return NULL;
  }
  CacheEntry *e = &cache->entries[cache->next++ % CACHED];
  if (e->object) {
    release(e->object);
    tz_free(e->key);
  }
  e->object = object;
  e->key = (uint16_t *)tz_alloc(key.length * sizeof(uint16_t));
  memcpy(e->key, key.units, key.length * sizeof(uint16_t));
  e->length = key.length;
  key_free(&key);

  return object;
}

void tz_date_format_shutdown(void);

void tz_intl_shutdown(void) {
  for (int32_t t = 0; t < tz_thread_total(); ++t) {
    TzThread *thread = tz_thread_at(t);
    Cache *cache = thread ? (Cache *)thread->caches[TZ_CACHE_INTL] : NULL;
    if (!cache) {
      continue;
    }
    for (int i = 0; i < CACHED; ++i) {
      CacheEntry *e = &cache->entries[i];
      if (e->object) {
        release(e->object);
        tz_free(e->key);
        e->object = NULL;
      }
    }
  }
  tz_date_format_shutdown();
}

int32_t tz_intl_text(TzStr *out, const TzIntl *o, int32_t which) {
  const char *text = which < 0 ? o->locale : o->text[which];
  *out = tz_str_from_ascii(text, strlen(text));

  return text[0] != 0;
}

double tz_intl_number(const TzIntl *o, int32_t which) { return o->number[which]; }

int32_t tz_intl_grouping(TzStr *out, const TzIntl *o) {
  const char *text = o->text[TZ_IO_USE_GROUPING];
  int on = strcmp(text, "false") != 0;
  *out = on ? tz_str_from_ascii(text, strlen(text)) : tz_str_from_ascii("", 0);

  return on;
}

static void *field_at(void *object, const TzIntlShape *shape, int field) {
  return (char *)object + shape->offsets[field];
}

void tz_intl_push_part(TzArray *parts, const TzIntlShape *shape, const char *type,
                       const TzUChar *value, int32_t length, const char *unit) {
  TzInstance *part = (TzInstance *)tz_obj_alloc(shape->size);
  memset(part, 0, (size_t)shape->size);
  part->rc = 1;
  part->layout = shape->layout;
  *(TzStr *)field_at(part, shape, TZ_IF_TYPE) = tz_str_from_ascii(type, strlen(type));
  *(TzStr *)field_at(part, shape, TZ_IF_VALUE) = tz_str_from_utf16(value, (size_t)length);
  if (unit && shape->offsets[TZ_IF_UNIT] >= 0) {
    TzOptStr *slot = (TzOptStr *)field_at(part, shape, TZ_IF_UNIT);
    slot->value = tz_str_from_ascii(unit, strlen(unit));
    slot->has = 1;
  }
  tz_arr_push_value(parts, &part);
}

typedef struct {
  int32_t field, start, limit;
} Span;

void tz_intl_push_value_parts(TzArray *parts, const TzIntlShape *shape, const void *value,
                              int32_t category, const char *(*type_of)(int32_t field, void *ctx),
                              void *ctx, const char *unit) {
  const TzIcu *icu = tz_icu();
  TzUErrorCode status = 0;
  int32_t length = 0;
  const TzUChar *text = icu->ufmtval_getString(value, &length, &status);
  if (TZ_U_FAILURE(status) || !text) {
    return;
  }
  Span local[32];
  Span *spans = local;
  int32_t count = 0, capacity = 32;
  void *position = icu->ucfpos_open(&status);
  icu->ucfpos_constrainCategory(position, category, &status);
  while (!TZ_U_FAILURE(status) && icu->ufmtval_nextPosition(value, position, &status)) {
    if (count == capacity) {
      Span *grown = (Span *)tz_alloc((size_t)capacity * 2 * sizeof(Span));
      memcpy(grown, spans, (size_t)count * sizeof(Span));
      if (spans != local) {
        tz_free(spans);
      }
      spans = grown;
      capacity *= 2;
    }
    Span *s = &spans[count++];
    s->field = icu->ucfpos_getField(position, &status);
    icu->ucfpos_getIndexes(position, &s->start, &s->limit, &status);
  }
  icu->ucfpos_close(position);
  int32_t from = 0, owner = -2;
  for (int32_t i = 0; i <= length; ++i) {
    int32_t best = -1;
    if (i < length) {
      for (int32_t k = 0; k < count; ++k) {
        if (spans[k].start <= i && i < spans[k].limit &&
            (best < 0 ||
             spans[k].limit - spans[k].start <= spans[best].limit - spans[best].start)) {
          best = k;
        }
      }
    }
    if (i < length && best == owner) {
      continue;
    }
    if (i > from) {
      if (owner >= 0) {
        tz_intl_push_part(parts, shape, type_of(spans[owner].field, ctx), text + from, i - from,
                          unit);
      } else {
        tz_intl_push_part(parts, shape, "literal", text + from, i - from, NULL);
      }
    }
    from = i;
    owner = best;
  }
  if (spans != local) {
    tz_free(spans);
  }
}

static int locale_tag(const TzIcu *icu, TzStr given, char *tag, int32_t size) {
  char id[160], canonical[160];
  if (!icu || !parse_tag(icu, given, id, sizeof id)) {
    return 0;
  }
  canonical_id(icu, id, canonical, sizeof canonical);
  TzUErrorCode status = 0;
  int32_t n = icu->uloc_toLanguageTag(canonical, tag, size - 1, 1, &status);
  if (TZ_U_FAILURE(status) || n <= 0) {
    return 0;
  }
  tag[n] = 0;

  return 1;
}

void tz_intl_locale_tag(TzStr *out, void *tag_obj, uint64_t tag_meta, TzMaker make) {
  (void)make;
  char tag[160];
  TzStr given = {tag_obj, tag_meta};
  *out = locale_tag(tz_icu(), given, tag, sizeof tag) ? tz_str_from_ascii(tag, strlen(tag))
                                                      : tz_str_from_ascii("", 0);
}

static size_t subtag(const char *tag, size_t at, size_t *length) {
  size_t end = at;
  while (tag[end] && tag[end] != '-') {
    ++end;
  }
  *length = end - at;

  return tag[end] ? end + 1 : end;
}

void tz_intl_locale_part(TzStr *out, void *tag_obj, uint64_t tag_meta, int64_t part, TzMaker make) {
  (void)make;
  const TzIcu *icu = tz_icu();
  *out = tz_str_from_ascii("", 0);
  char tag[160], id[160], base[160], text[160];
  TzStr given = {tag_obj, tag_meta};
  int32_t parsed = 0;
  TzUErrorCode status = 0;
  if (!icu || !tz_intl_ascii(given, tag, sizeof tag)) {
    return;
  }
  int32_t n = icu->uloc_forLanguageTag(tag, id, (int32_t)sizeof id - 1, &parsed, &status);
  if (TZ_U_FAILURE(status) || n <= 0) {
    return;
  }
  id[n] = 0;
  if (part == 4) {
    status = 0;
    n = icu->uloc_addLikelySubtags(id, base, (int32_t)sizeof base - 1, &status);
    if (TZ_U_FAILURE(status) || n <= 0) {
      return;
    }
    base[n] = 0;
    status = 0;
    n = icu->uloc_toLanguageTag(base, text, (int32_t)sizeof text - 1, 1, &status);
    if (!TZ_U_FAILURE(status) && n > 0) {
      *out = tz_str_from_ascii(text, (size_t)n);
    }
    return;
  }
  status = 0;
  n = icu->uloc_getBaseName(id, base, (int32_t)sizeof base - 1, &status);
  if (TZ_U_FAILURE(status) || n <= 0) {
    return;
  }
  base[n] = 0;
  status = 0;
  n = icu->uloc_toLanguageTag(base, text, (int32_t)sizeof text - 1, 1, &status);
  if (TZ_U_FAILURE(status) || n <= 0) {
    return;
  }
  text[n] = 0;
  if (part == 0) {
    *out = tz_str_from_ascii(text, (size_t)n);
    return;
  }
  size_t length = 0, at = subtag(text, 0, &length);
  if (part == 1) {
    *out = tz_str_from_ascii(text, length);
    return;
  }
  while (text[at]) {
    size_t from = at;
    at = subtag(text, at, &length);
    int alpha = 1, digits = 1;
    for (size_t i = 0; i < length; ++i) {
      char c = text[from + i];
      alpha &= (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
      digits &= c >= '0' && c <= '9';
    }
    if (part == 2 && length == 4 && alpha) {
      *out = tz_str_from_ascii(text + from, length);
      return;
    }
    if (part == 3 && ((length == 2 && alpha) || (length == 3 && digits))) {
      *out = tz_str_from_ascii(text + from, length);
      return;
    }
  }
}

static int canonical_list(const TzIcu *icu, const TzArray *locales, TzArray *out) {
  for (uint32_t i = 0; i < locales->length; ++i) {
    TzStr s = ((const TzStr *)locales->data)[i];
    char id[160], canonical[160], tag[160];
    if (!parse_tag(icu, s, id, sizeof id)) {
      tz_panicf("internal error: a language tag that is not one");
    }
    canonical_id(icu, id, canonical, sizeof canonical);
    TzUErrorCode status = 0;
    int32_t n = icu->uloc_toLanguageTag(canonical, tag, (int32_t)sizeof tag - 1, 1, &status);
    if (TZ_U_FAILURE(status) || n <= 0) {
      tz_panicf("internal error: a language tag that is not one");
    }
    tag[n] = 0;
    int seen = 0;
    for (uint32_t k = 0; k < out->length && !seen; ++k) {
      TzStr t = ((const TzStr *)out->data)[k];
      char text[160];
      seen = tz_intl_ascii(t, text, sizeof text) && strcmp(text, tag) == 0;
    }
    if (!seen) {
      TzStr value = tz_str_from_ascii(tag, (size_t)n);
      tz_arr_push_value(out, &value);
    }
  }

  return 1;
}

TzArray *tz_intl_canonical_locales(const TzArray *locales) {
  const TzIcu *icu = tz_intl_icu();
  TzArray *out = tz_arr_new(&tz_string_type, 0, locales->length);
  if (!canonical_list(icu, locales, out)) {
    tz_arr_free(out);
    return NULL;
  }

  return out;
}

TzArray *tz_intl_supported_locales(const TzArray *locales, const TzIntlOptions *o,
                                   const char *method) {
  const TzIcu *icu = tz_intl_icu();
  static const char *const matchers[] = {"lookup", "best fit", NULL};
  char buffer[TZ_INTL_TEXT];
  int failed = 0;
  TzArray *all = tz_arr_new(&tz_string_type, 0, locales->length);
  int ok = canonical_list(icu, locales, all);
  if (ok) {
    tz_intl_option(o, TZ_IO_LOCALE_MATCHER, matchers, buffer, method, &failed);
  }
  TzArray *out = tz_arr_new(&tz_string_type, 0, all->length);
  for (uint32_t k = 0; k < all->length; ++k) {
    TzStr tag = ((TzStr *)all->data)[k];
    char text[160], id[160], base[64], found[64];
    int32_t parsed;
    TzUErrorCode status = 0;
    int supported = 0;
    if (ok && !failed && tz_intl_ascii(tag, text, sizeof text)) {
      int32_t n = icu->uloc_forLanguageTag(text, id, (int32_t)sizeof id - 1, &parsed, &status);
      if (!TZ_U_FAILURE(status) && n >= 0) {
        id[n] = 0;
        status = 0;
        n = icu->uloc_getBaseName(id, base, (int32_t)sizeof base - 1, &status);
        base[TZ_U_FAILURE(status) || n < 0 ? 0 : n] = 0;
        best_available(icu, base, found, sizeof found);
        supported = found[0] != 0;
      }
    }
    if (supported) {
      tz_arr_push_value(out, &tag);
    } else {
      tz_str_release_value(tag);
    }
  }
  all->length = 0;
  tz_arr_free(all);
  if (!ok || failed) {
    tz_arr_free(out);
    return NULL;
  }

  return out;
}
