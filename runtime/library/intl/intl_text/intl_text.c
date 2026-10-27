#include "runtime/library/intl/intl/intl.h"
#include "runtime/core/output/output.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/array/array.h"
#include "runtime/core/closure/closure.h"
#include "runtime/core/error/error.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include <math.h>
#include <string.h>

enum {
  UCOL_ALTERNATE_HANDLING = 1,
  UCOL_CASE_FIRST = 2,
  UCOL_CASE_LEVEL = 3,
  UCOL_NORMALIZATION_MODE = 4,
  UCOL_STRENGTH = 5,
  UCOL_NUMERIC_COLLATION = 7,
  UCOL_PRIMARY = 0,
  UCOL_SECONDARY = 1,
  UCOL_TERTIARY = 2,
  UCOL_OFF = 16,
  UCOL_ON = 17,
  UCOL_SHIFTED = 20,
  UCOL_NON_IGNORABLE = 21,
  UCOL_LOWER_FIRST = 24,
  UCOL_UPPER_FIRST = 25,
};

enum { UBRK_CHARACTER = 0, UBRK_WORD = 1, UBRK_SENTENCE = 3, UBRK_DONE = -1 };
#define WORD_NONE_LIMIT 100

typedef struct {
  TzIntl base;
  void *collator;
} TzCollator;

typedef struct {
  TzIntl base;
  int32_t type;
  char icu_locale[64];
} TzSegmenter;

typedef struct {
  int64_t rc;
  const TzLayout *layout;
  TzStr input;
  TzUChar *units;
  int32_t length;
  int32_t type;
  void *iterator;
} TzSegments;

static void collator_drop(void *object);
static void segmenter_drop(void *object);
static void segments_drop(void *object);
static const TzLayout collator_layout = {collator_drop, NULL, 0, TZ_LAYOUT_INTL, "Collator", 8, 0};
static const TzLayout segmenter_layout = {segmenter_drop, NULL, 0, TZ_LAYOUT_INTL,
                                          "Segmenter",    9,    0};
static const TzLayout segments_layout = {segments_drop, NULL, 0, TZ_LAYOUT_INTL, NULL, 0, 0};

static const char *const matchers[] = {"lookup", "best fit", NULL};

static int has_collation(const TzIcu *icu, const char *locale, const char *collation) {
  if (strcmp(collation, "standard") == 0 || strcmp(collation, "search") == 0) {
    return 0;
  }
  const char *legacy = icu->uloc_toLegacyType("co", collation);
  if (!legacy) {
    legacy = collation;
  }
  TzUErrorCode status = 0;
  void *values = icu->ucol_getKeywordValuesForLocale("collation", locale, 0, &status);
  if (TZ_U_FAILURE(status) || !values) {
    return 0;
  }
  int found = 0;
  int32_t length;
  const char *value;
  while (!found && (value = icu->uenum_next(values, &length, &status)) != NULL) {
    found = strcmp(value, legacy) == 0;
  }
  icu->uenum_close(values);

  return found;
}

static void append_keyword(char *tag, size_t size, int *opened, const char *key,
                           const char *value) {
  if (strlen(tag) + strlen(key) + strlen(value) + 6 >= size) {
    return;
  }
  strcat(tag, *opened ? "-" : "-u-");
  *opened = 1;
  strcat(tag, key);
  if (value[0]) {
    strcat(tag, "-");
    strcat(tag, value);
  }
}

TzIntl *tz_collator_create(const TzIntlOptions *o, const char *method) {
  const TzIcu *icu = tz_intl_icu();
  char requested[160], found[64];
  if (!tz_intl_resolve_locale(icu, o->has_locale, o->locale, requested, found)) {
    return NULL;
  }
  static const char *const usages[] = {"sort", "search", NULL};
  static const char *const case_firsts[] = {"upper", "lower", "false", NULL};
  static const char *const sensitivities[] = {"base", "accent", "case", "variant", NULL};
  char b[5][TZ_INTL_TEXT];
  int failed = 0;
  const char *usage = tz_intl_option(o, TZ_IO_USAGE, usages, b[0], method, &failed);
  tz_intl_option(o, TZ_IO_LOCALE_MATCHER, matchers, b[1], method, &failed);
  const char *collation = tz_intl_option(o, TZ_IO_COLLATION, NULL, b[2], method, &failed);
  if (failed) {
    return NULL;
  }
  if (collation && !tz_intl_is_keyword_type(collation)) {
    collation = NULL;
  }
  int numeric = tz_intl_bool_option(o, TZ_IO_NUMERIC);
  const char *case_first = tz_intl_option(o, TZ_IO_CASE_FIRST, case_firsts, b[3], method, &failed);
  if (failed) {
    return NULL;
  }
  usage = usage ? usage : "sort";
  int search = strcmp(usage, "search") == 0;

  char keyword_co[TZ_INTL_TEXT], keyword_kf[TZ_INTL_TEXT], keyword_kn[TZ_INTL_TEXT];
  tz_intl_keyword(icu, requested, "co", keyword_co, sizeof keyword_co);
  tz_intl_keyword(icu, requested, "kf", keyword_kf, sizeof keyword_kf);
  tz_intl_keyword(icu, requested, "kn", keyword_kn, sizeof keyword_kn);
  int has_kn = 0;
  {
    char raw[16];
    TzUErrorCode status = 0;
    int32_t n =
        icu->uloc_getKeywordValue(requested, "colnumeric", raw, (int32_t)sizeof raw - 1, &status);
    has_kn = !TZ_U_FAILURE(status) && n > 0;
  }
  int kn_valid = has_kn && (strcmp(keyword_kn, "true") == 0 || strcmp(keyword_kn, "false") == 0 ||
                            strcmp(keyword_kn, "yes") == 0 || strcmp(keyword_kn, "no") == 0);
  int kn_true = kn_valid && (strcmp(keyword_kn, "true") == 0 || strcmp(keyword_kn, "yes") == 0);
  int kf_valid = strcmp(keyword_kf, "upper") == 0 || strcmp(keyword_kf, "lower") == 0 ||
                 strcmp(keyword_kf, "false") == 0;
  const char *chosen = NULL;
  if (!search) {
    if (collation && has_collation(icu, found, collation)) {
      chosen = collation;
    } else if (keyword_co[0] && has_collation(icu, found, keyword_co)) {
      chosen = keyword_co;
    }
  }

  TzCollator *c =
      (TzCollator *)tz_intl_alloc(sizeof(TzCollator), TZ_INTL_COLLATOR, &collator_layout);
  TzIntl *r = &c->base;
  char tag[TZ_INTL_LOCALE];
  int opened = 0;
  tz_intl_tag(icu, found, tag, sizeof tag);
  if (chosen) {
    append_keyword(tag, sizeof tag, &opened, "co", chosen);
  }
  if (kf_valid) {
    append_keyword(tag, sizeof tag, &opened, "kf", keyword_kf);
  }
  if (kn_valid) {
    append_keyword(tag, sizeof tag, &opened, "kn", kn_true ? "" : "false");
  }
  tz_intl_copy(r->locale, sizeof r->locale, tag);
  tz_intl_set_text(r, TZ_IO_USAGE, usage);
  tz_intl_set_text(r, TZ_IO_COLLATION, chosen ? chosen : "default");

  char id[160];
  tz_intl_copy(id, sizeof id, found);
  TzUErrorCode status = 0;
  if (search || chosen) {
    const char *legacy = search ? "search" : icu->uloc_toLegacyType("co", chosen);
    icu->uloc_setKeywordValue("collation", legacy ? legacy : chosen, id, (int32_t)sizeof id,
                              &status);
    status = 0;
  }
  c->collator = icu->ucol_open(id, &status);
  if (TZ_U_FAILURE(status) || !c->collator) {
    tz_intl_no_icu();
  }
  void *coll = c->collator;
  status = 0;
  if (numeric >= 0 || kn_valid) {
    icu->ucol_setAttribute(coll, UCOL_NUMERIC_COLLATION,
                           (numeric >= 0 ? numeric : kn_true) ? UCOL_ON : UCOL_OFF, &status);
  }
  const char *first = case_first ? case_first : kf_valid ? keyword_kf : NULL;
  if (first) {
    icu->ucol_setAttribute(coll, UCOL_CASE_FIRST,
                           strcmp(first, "upper") == 0   ? UCOL_UPPER_FIRST
                           : strcmp(first, "lower") == 0 ? UCOL_LOWER_FIRST
                                                         : UCOL_OFF,
                           &status);
  }
  icu->ucol_setAttribute(coll, UCOL_NORMALIZATION_MODE, UCOL_ON, &status);
  const char *sensitivity =
      tz_intl_option(o, TZ_IO_SENSITIVITY, sensitivities, b[4], method, &failed);
  int ignore_punctuation = failed ? -1 : tz_intl_bool_option(o, TZ_IO_IGNORE_PUNCTUATION);
  if (failed) {
    collator_drop(c);
    return NULL;
  }
  sensitivity = sensitivity ? sensitivity : "variant";
  status = 0;
  if (strcmp(sensitivity, "base") == 0) {
    icu->ucol_setAttribute(coll, UCOL_STRENGTH, UCOL_PRIMARY, &status);
  } else if (strcmp(sensitivity, "accent") == 0) {
    icu->ucol_setAttribute(coll, UCOL_STRENGTH, UCOL_SECONDARY, &status);
  } else if (strcmp(sensitivity, "case") == 0) {
    icu->ucol_setAttribute(coll, UCOL_STRENGTH, UCOL_PRIMARY, &status);
    icu->ucol_setAttribute(coll, UCOL_CASE_LEVEL, UCOL_ON, &status);
  } else {
    icu->ucol_setAttribute(coll, UCOL_STRENGTH, UCOL_TERTIARY, &status);
  }
  if (ignore_punctuation >= 0) {
    icu->ucol_setAttribute(coll, UCOL_ALTERNATE_HANDLING,
                           ignore_punctuation ? UCOL_SHIFTED : UCOL_NON_IGNORABLE, &status);
  }
  tz_intl_set_text(r, TZ_IO_SENSITIVITY, sensitivity);
  status = 0;
  r->number[TZ_IO_IGNORE_PUNCTUATION] =
      icu->ucol_getAttribute(coll, UCOL_ALTERNATE_HANDLING, &status) == UCOL_SHIFTED;
  r->number[TZ_IO_NUMERIC] =
      icu->ucol_getAttribute(coll, UCOL_NUMERIC_COLLATION, &status) == UCOL_ON;
  int resolved_first = icu->ucol_getAttribute(coll, UCOL_CASE_FIRST, &status);
  tz_intl_set_text(r, TZ_IO_CASE_FIRST,
                   resolved_first == UCOL_UPPER_FIRST   ? "upper"
                   : resolved_first == UCOL_LOWER_FIRST ? "lower"
                                                        : "false");

  return r;
}

static void collator_drop(void *object) {
  TzCollator *c = (TzCollator *)object;
  if (c->collator) {
    tz_icu()->ucol_close(c->collator);
  }
  tz_obj_free(c);
}

static int32_t compare(const void *collator, TzStr a, TzStr b) {
  TzUChar buffer_a[128], buffer_b[128], *heap_a, *heap_b;
  int32_t length_a, length_b;
  const TzUChar *units_a = tz_intl_utf16(&a, buffer_a, 128, &heap_a, &length_a);
  const TzUChar *units_b = tz_intl_utf16(&b, buffer_b, 128, &heap_b, &length_b);
  int32_t result = tz_icu()->ucol_strcoll(collator, units_a, length_a, units_b, length_b);
  tz_free(heap_a);
  tz_free(heap_b);

  return result;
}

int32_t tz_collator_compare(TzCollator *c, void *a_obj, uint64_t a_meta, void *b_obj,
                            uint64_t b_meta) {
  TzStr a = {a_obj, a_meta}, b = {b_obj, b_meta};

  return compare(c->collator, a, b);
}

int32_t tz_str_locale_compare(void *a_obj, uint64_t a_meta, void *b_obj, uint64_t b_meta,
                              const TzIntlOptions *o) {
  TzIntl *c = tz_intl_cached(TZ_INTL_COLLATOR, o, "String.prototype.localeCompare");
  if (!c) {
    return 0;
  }
  TzStr a = {a_obj, a_meta}, b = {b_obj, b_meta};

  return compare(((TzCollator *)c)->collator, a, b);
}

void tz_str_normalize(TzStr *out, void *obj, uint64_t meta, int32_t has_form, void *form_obj,
                      uint64_t form_meta) {
  TzStr s = {obj, meta};
  int form = 0;
  if (has_form) {
    static const char *const forms[] = {"NFC", "NFD", "NFKC", "NFKD"};
    TzStr f = {form_obj, form_meta};
    char text[8];
    form = -1;
    if (tz_intl_ascii(f, text, sizeof text)) {
      for (int i = 0; i < 4; ++i) {
        if (strcmp(forms[i], text) == 0) {
          form = i;
        }
      }
    }
    if (form < 0) {
      tz_panicf("internal error: a normalization form that is not one");
    }
  }
  uint32_t n;
  int two;
  const void *units = tz_str_units(&s, &n, &two);
  int ascii = !two;
  for (uint32_t i = 0; ascii && i < n; ++i) {
    ascii = ((const uint8_t *)units)[i] < 0x80;
  }
  if (ascii) {
    tz_str_retain_value(s);
    *out = s;
    return;
  }
  const TzIcu *icu = tz_intl_icu();
  TzUErrorCode status = 0;
  const void *normalizer = form == 0   ? icu->unorm2_getNFCInstance(&status)
                           : form == 1 ? icu->unorm2_getNFDInstance(&status)
                           : form == 2 ? icu->unorm2_getNFKCInstance(&status)
                                       : icu->unorm2_getNFKDInstance(&status);
  if (TZ_U_FAILURE(status)) {
    tz_intl_no_icu();
  }
  TzUChar buffer[256], *heap;
  int32_t length;
  const TzUChar *text = tz_intl_utf16(&s, buffer, 256, &heap, &length);
  if (icu->unorm2_spanQuickCheckYes(normalizer, text, length, &status) == length &&
      !TZ_U_FAILURE(status)) {
    tz_free(heap);
    tz_str_retain_value(s);
    *out = s;
    return;
  }
  status = 0;
  TzUChar local[256];
  int32_t capacity = 256;
  TzUChar *result = local;
  int32_t made = icu->unorm2_normalize(normalizer, text, length, result, capacity, &status);
  if (status == TZ_U_BUFFER_OVERFLOW_ERROR) {
    capacity = made + 1;
    result = (TzUChar *)tz_alloc((size_t)capacity * sizeof(TzUChar));
    status = 0;
    made = icu->unorm2_normalize(normalizer, text, length, result, capacity, &status);
  }
  *out = TZ_U_FAILURE(status) ? tz_str_from_ascii("", 0) : tz_str_from_utf16(result, (size_t)made);
  if (result != local) {
    tz_free(result);
  }
  tz_free(heap);
}

TzIntl *tz_seg_create(const TzIntlOptions *o) {
  const char *method = "Intl.Segmenter";
  const TzIcu *icu = tz_intl_icu();
  char requested[160], found[64];
  if (!tz_intl_resolve_locale(icu, o->has_locale, o->locale, requested, found)) {
    return NULL;
  }
  static const char *const granularities[] = {"grapheme", "word", "sentence", NULL};
  char b[2][TZ_INTL_TEXT];
  int failed = 0;
  tz_intl_option(o, TZ_IO_LOCALE_MATCHER, matchers, b[0], method, &failed);
  const char *granularity =
      tz_intl_option(o, TZ_IO_GRANULARITY, granularities, b[1], method, &failed);
  if (failed) {
    return NULL;
  }
  granularity = granularity ? granularity : "grapheme";
  TzSegmenter *s =
      (TzSegmenter *)tz_intl_alloc(sizeof(TzSegmenter), TZ_INTL_SEGMENTER, &segmenter_layout);
  tz_intl_tag(icu, found, s->base.locale, sizeof s->base.locale);
  tz_intl_set_text(&s->base, TZ_IO_GRANULARITY, granularity);
  tz_intl_copy(s->icu_locale, sizeof s->icu_locale, found);
  s->type = strcmp(granularity, "word") == 0       ? UBRK_WORD
            : strcmp(granularity, "sentence") == 0 ? UBRK_SENTENCE
                                                   : UBRK_CHARACTER;

  return &s->base;
}

static void segmenter_drop(void *object) { tz_obj_free(object); }

void *tz_seg_segment(TzSegmenter *s, void *obj, uint64_t meta) {
  const TzIcu *icu = tz_icu();
  TzSegments *segments = (TzSegments *)tz_obj_alloc(sizeof(TzSegments));
  memset(segments, 0, sizeof *segments);
  segments->rc = 1;
  segments->layout = &segments_layout;
  TzStr input = {obj, meta};
  tz_str_retain_value(input);
  segments->input = input;
  segments->type = s->type;
  uint32_t n;
  int two;
  const void *units = tz_str_units(&input, &n, &two);
  segments->units = (TzUChar *)tz_alloc(((size_t)n + 1) * sizeof(TzUChar));
  for (uint32_t i = 0; i < n; ++i) {
    segments->units[i] = two ? ((const uint16_t *)units)[i] : ((const uint8_t *)units)[i];
  }
  segments->length = (int32_t)n;
  TzUErrorCode status = 0;
  segments->iterator =
      icu->ubrk_open(s->type, s->icu_locale, segments->units, segments->length, &status);
  if (TZ_U_FAILURE(status) || !segments->iterator) {
    tz_intl_no_icu();
  }

  return segments;
}

static void segments_drop(void *object) {
  TzSegments *s = (TzSegments *)object;
  if (s->iterator) {
    tz_icu()->ubrk_close(s->iterator);
  }
  tz_str_release_value(s->input);
  tz_free(s->units);
  tz_obj_free(s);
}

static void *segment_data(TzSegments *s, int32_t start, int32_t end, const TzIntlShape *shape) {
  TzInstance *data = (TzInstance *)tz_obj_alloc(shape->size);
  memset(data, 0, (size_t)shape->size);
  data->rc = 1;
  data->layout = shape->layout;
  char *base = (char *)data;
  *(TzStr *)(base + shape->offsets[TZ_IF_SEGMENT]) =
      tz_str_sub(s->input, (uint32_t)start, (uint32_t)end);
  *(int64_t *)(base + shape->offsets[TZ_IF_INDEX]) = start;
  tz_str_retain_value(s->input);
  *(TzStr *)(base + shape->offsets[TZ_IF_INPUT]) = s->input;
  if (s->type == UBRK_WORD && shape->offsets[TZ_IF_WORD_LIKE] >= 0) {
    uint8_t *word_like = (uint8_t *)(base + shape->offsets[TZ_IF_WORD_LIKE]);
    word_like[0] = tz_icu()->ubrk_getRuleStatus(s->iterator) >= WORD_NONE_LIMIT;
    word_like[1] = 1;
  }

  return data;
}

TzArray *tz_segments_array(TzSegments *s, const TzIntlShape *shape) {
  const TzIcu *icu = tz_icu();
  TzArray *out = tz_arr_new(shape->type, 0, 8);
  int32_t start = icu->ubrk_first(s->iterator), end;
  while ((end = icu->ubrk_next(s->iterator)) != UBRK_DONE) {
    void *data = segment_data(s, start, end, shape);
    tz_arr_push_value(out, &data);
    start = end;
  }

  return out;
}

void *tz_segments_containing(TzSegments *s, double index, const TzIntlShape *shape) {
  double n = isnan(index) ? 0 : trunc(index);
  if (n < 0 || n >= s->length) {
    return NULL;
  }
  const TzIcu *icu = tz_icu();
  int32_t at = (int32_t)n;
  int32_t start = icu->ubrk_isBoundary(s->iterator, at) ? at : icu->ubrk_preceding(s->iterator, at);
  int32_t end = icu->ubrk_following(s->iterator, at);

  return segment_data(s, start, end, shape);
}
