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
  UFIELD_CATEGORY_NUMBER = 2,
  UFIELD_CATEGORY_LIST = 3,
  ULISTFMT_LITERAL_FIELD = 0,
  ULISTFMT_ELEMENT_FIELD = 1,
  UDISPCTX_STANDARD_NAMES = 0,
  UDISPCTX_DIALECT_NAMES = 1,
  UDISPCTX_CAPITALIZATION_NONE = 256,
  UDISPCTX_LENGTH_FULL = 512,
  UDISPCTX_LENGTH_SHORT = 513,
  UDISPCTX_SUBSTITUTE = 768,
  UDISPCTX_NO_SUBSTITUTE = 769,
  UCURR_LONG_NAME = 1,
  U_USING_DEFAULT_WARNING = -127,
  U_ILLEGAL_ARGUMENT_ERROR = 1,
};

typedef struct {
  TzIntl base;
  void *formatter;
} TzRelativeTimeFormat;

typedef struct {
  TzIntl base;
  void *formatter;
} TzListFormat;

typedef struct {
  TzIntl base;
  void *names;
  void *generator;
  char icu_locale[64];
} TzDisplayNames;

static void rtf_drop(void *object);
static void lf_drop(void *object);
static void dn_drop(void *object);
static const TzLayout rtf_layout = {rtf_drop, NULL, 0, TZ_LAYOUT_INTL, "RelativeTimeFormat", 18, 0};
static const TzLayout lf_layout = {lf_drop, NULL, 0, TZ_LAYOUT_INTL, "ListFormat", 10, 0};
static const TzLayout dn_layout = {dn_drop, NULL, 0, TZ_LAYOUT_INTL, "DisplayNames", 12, 0};

static const char *const matchers[] = {"lookup", "best fit", NULL};
static const char *const widths[] = {"long", "short", "narrow", NULL};

static int width_of(const char *style) {
  return strcmp(style, "short") == 0 ? 1 : strcmp(style, "narrow") == 0 ? 2 : 0;
}

static TzStr text_of(const TzIcu *icu, const void *value, TzUErrorCode *status) {
  int32_t length = 0;
  const TzUChar *text =
      TZ_U_FAILURE(*status) ? NULL : icu->ufmtval_getString(value, &length, status);

  return text && !TZ_U_FAILURE(*status) ? tz_str_from_utf16(text, (size_t)length)
                                        : tz_str_from_ascii("", 0);
}

TzIntl *tz_rtf_create(const TzIntlOptions *o) {
  const char *method = "Intl.RelativeTimeFormat";
  const TzIcu *icu = tz_intl_icu();
  char requested[160], found[64];
  if (!tz_intl_resolve_locale(icu, o->has_locale, o->locale, requested, found)) {
    return NULL;
  }
  static const char *const numerics[] = {"always", "auto", NULL};
  char b[4][TZ_INTL_TEXT];
  int failed = 0;
  tz_intl_option(o, TZ_IO_LOCALE_MATCHER, matchers, b[0], method, &failed);
  const char *numbering = tz_intl_option(o, TZ_IO_NUMBERING_SYSTEM, NULL, b[1], method, &failed);
  if (failed) {
    return NULL;
  }
  if (numbering && !tz_intl_is_keyword_type(numbering)) {
    numbering = NULL;
  }
  const char *style = tz_intl_option(o, TZ_IO_STYLE, widths, b[2], method, &failed);
  const char *numeric = tz_intl_option(o, TZ_IO_NUMERIC, numerics, b[3], method, &failed);
  if (failed) {
    return NULL;
  }
  style = style ? style : "long";
  numeric = numeric ? numeric : "always";
  TzRelativeTimeFormat *f = (TzRelativeTimeFormat *)tz_intl_alloc(
      sizeof(TzRelativeTimeFormat), TZ_INTL_RELATIVE_TIME_FORMAT, &rtf_layout);
  TzIntl *r = &f->base;
  char keyword[TZ_INTL_TEXT], tag[TZ_INTL_LOCALE], id[160];
  tz_intl_keyword(icu, requested, "nu", keyword, sizeof keyword);
  int keyword_ok = keyword[0] && tz_intl_has_numbering(icu, keyword);
  const char *system = NULL;
  int in_tag = 0;
  if (numbering && tz_intl_has_numbering(icu, numbering)) {
    system = numbering;
    in_tag = keyword_ok && strcmp(keyword, numbering) == 0;
  } else if (keyword_ok) {
    system = keyword;
    in_tag = 1;
  }
  tz_intl_tag(icu, found, tag, sizeof tag);
  if (in_tag && strlen(tag) + strlen(system) + 7 < sizeof tag) {
    strcat(tag, "-u-nu-");
    strcat(tag, system);
  }
  tz_intl_copy(r->locale, sizeof r->locale, tag);
  tz_intl_copy(id, sizeof id, found);
  TzUErrorCode status = 0;
  if (system) {
    icu->uloc_setKeywordValue("numbers", system, id, (int32_t)sizeof id, &status);
    status = 0;
  }
  void *numbers = icu->unumsys_open(id, &status);
  tz_intl_set_text(r, TZ_IO_NUMBERING_SYSTEM,
                   !TZ_U_FAILURE(status) && numbers ? icu->unumsys_getName(numbers) : "latn");
  if (numbers) {
    icu->unumsys_close(numbers);
  }
  tz_intl_set_text(r, TZ_IO_STYLE, style);
  tz_intl_set_text(r, TZ_IO_NUMERIC, numeric);
  status = 0;
  f->formatter =
      icu->ureldatefmt_open(id, NULL, width_of(style), UDISPCTX_CAPITALIZATION_NONE, &status);
  if (TZ_U_FAILURE(status) || !f->formatter) {
    tz_intl_no_icu();
  }

  return r;
}

static void rtf_drop(void *object) {
  TzRelativeTimeFormat *f = (TzRelativeTimeFormat *)object;
  if (f->formatter) {
    tz_icu()->ureldatefmt_close(f->formatter);
  }
  tz_obj_free(f);
}

static int rtf_unit(TzStr unit, const char **singular) {
  static const char *const units[] = {"year", "quarter", "month",  "week",
                                      "day",  "hour",    "minute", "second"};
  char text[16];
  if (!tz_intl_ascii(unit, text, sizeof text)) {
    return -1;
  }
  size_t n = strlen(text);
  if (n > 1 && text[n - 1] == 's') {
    text[n - 1] = 0;
  }
  for (int i = 0; i < 8; ++i) {
    if (strcmp(units[i], text) == 0) {
      *singular = units[i];
      return i;
    }
  }

  return -1;
}

static void *rtf_result(TzRelativeTimeFormat *f, double value, void *unit_obj, uint64_t unit_meta,
                        const char *method, const char **singular) {
  TzStr unit = {unit_obj, unit_meta};
  int which = rtf_unit(unit, singular);
  if (which < 0) {
    tz_panicf("internal error: a unit %s does not know", method);
  }
  if (!isfinite(value)) {
    tz_panicf("Value need to be finite number for %s", method);
  }
  const TzIcu *icu = tz_icu();
  TzUErrorCode status = 0;
  void *result = icu->ureldatefmt_openResult(&status);
  if (strcmp(f->base.text[TZ_IO_NUMERIC], "auto") == 0) {
    icu->ureldatefmt_formatToResult(f->formatter, value, which, result, &status);
  } else {
    icu->ureldatefmt_formatNumericToResult(f->formatter, value, which, result, &status);
  }

  return result;
}

void tz_rtf_format(TzStr *out, TzRelativeTimeFormat *f, double value, void *unit_obj,
                   uint64_t unit_meta) {
  const char *singular;
  void *result = rtf_result(f, value, unit_obj, unit_meta,
                            "Intl.RelativeTimeFormat.prototype.format()", &singular);
  if (!result) {
    *out = tz_str_from_ascii("", 0);
    return;
  }
  const TzIcu *icu = tz_icu();
  TzUErrorCode status = 0;
  *out = text_of(icu, icu->ureldatefmt_resultAsValue(result, &status), &status);
  icu->ureldatefmt_closeResult(result);
}

static const char *rtf_part_type(int32_t field, void *ctx) {
  (void)ctx;
  switch (field) {
  case 0:
    return "integer";
  case 1:
    return "fraction";
  case 2:
    return "decimal";
  case 6:
    return "group";
  case 10:
    return "minusSign";
  default:
    return "literal";
  }
}

TzArray *tz_rtf_format_to_parts(TzRelativeTimeFormat *f, double value, void *unit_obj,
                                uint64_t unit_meta, const TzIntlShape *shape) {
  const char *singular;
  void *result = rtf_result(f, value, unit_obj, unit_meta,
                            "Intl.RelativeTimeFormat.prototype.formatToParts()", &singular);
  if (!result) {
    return NULL;
  }
  const TzIcu *icu = tz_icu();
  TzUErrorCode status = 0;
  const void *formatted = icu->ureldatefmt_resultAsValue(result, &status);
  TzArray *parts = tz_arr_new(shape->type, 0, 4);
  if (!TZ_U_FAILURE(status)) {
    tz_intl_push_value_parts(parts, shape, formatted, UFIELD_CATEGORY_NUMBER, rtf_part_type, NULL,
                             singular);
  }
  icu->ureldatefmt_closeResult(result);

  return parts;
}

TzIntl *tz_lf_create(const TzIntlOptions *o) {
  const char *method = "Intl.ListFormat";
  const TzIcu *icu = tz_intl_icu();
  char requested[160], found[64];
  if (!tz_intl_resolve_locale(icu, o->has_locale, o->locale, requested, found)) {
    return NULL;
  }
  static const char *const types[] = {"conjunction", "disjunction", "unit", NULL};
  char b[3][TZ_INTL_TEXT];
  int failed = 0;
  tz_intl_option(o, TZ_IO_LOCALE_MATCHER, matchers, b[0], method, &failed);
  const char *type = tz_intl_option(o, TZ_IO_TYPE, types, b[1], method, &failed);
  const char *style = tz_intl_option(o, TZ_IO_STYLE, widths, b[2], method, &failed);
  if (failed) {
    return NULL;
  }
  type = type ? type : "conjunction";
  style = style ? style : "long";
  TzListFormat *f =
      (TzListFormat *)tz_intl_alloc(sizeof(TzListFormat), TZ_INTL_LIST_FORMAT, &lf_layout);
  TzIntl *r = &f->base;
  tz_intl_tag(icu, found, r->locale, sizeof r->locale);
  tz_intl_set_text(r, TZ_IO_TYPE, type);
  tz_intl_set_text(r, TZ_IO_STYLE, style);
  TzUErrorCode status = 0;
  int which = strcmp(type, "disjunction") == 0 ? 1 : strcmp(type, "unit") == 0 ? 2 : 0;
  f->formatter = icu->ulistfmt_openForType(found, which, width_of(style), &status);
  if (TZ_U_FAILURE(status) || !f->formatter) {
    tz_intl_no_icu();
  }

  return r;
}

static void lf_drop(void *object) {
  TzListFormat *f = (TzListFormat *)object;
  if (f->formatter) {
    tz_icu()->ulistfmt_close(f->formatter);
  }
  tz_obj_free(f);
}

static void *lf_result(TzListFormat *f, const TzArray *list, TzUErrorCode *status) {
  const TzIcu *icu = tz_icu();
  uint32_t n = list->length;
  const TzUChar **strings = (const TzUChar **)tz_alloc(((size_t)n + 1) * sizeof(TzUChar *));
  int32_t *lengths = (int32_t *)tz_alloc(((size_t)n + 1) * sizeof(int32_t));
  TzUChar **heaps = (TzUChar **)tz_alloc(((size_t)n + 1) * sizeof(TzUChar *));
  TzStr *items = (TzStr *)tz_alloc(((size_t)n + 1) * sizeof(TzStr));
  static const TzUChar empty[1] = {0};
  for (uint32_t i = 0; i < n; ++i) {
    items[i] = ((const TzStr *)list->data)[i];
    strings[i] = tz_intl_utf16(&items[i], NULL, 0, &heaps[i], &lengths[i]);
    if (!strings[i]) {
      strings[i] = empty;
    }
  }
  void *result = icu->ulistfmt_openResult(status);
  icu->ulistfmt_formatStringsToResult(f->formatter, strings, lengths, (int32_t)n, result, status);
  for (uint32_t i = 0; i < n; ++i) {
    tz_free(heaps[i]);
  }
  tz_free(heaps);
  tz_free(items);
  tz_free(lengths);
  tz_free(strings);

  return result;
}

void tz_lf_format(TzStr *out, TzListFormat *f, const TzArray *list) {
  const TzIcu *icu = tz_icu();
  TzUErrorCode status = 0;
  void *result = lf_result(f, list, &status);
  *out = text_of(icu, icu->ulistfmt_resultAsValue(result, &status), &status);
  icu->ulistfmt_closeResult(result);
}

static const char *lf_part_type(int32_t field, void *ctx) {
  (void)ctx;

  return field == ULISTFMT_ELEMENT_FIELD ? "element" : "literal";
}

TzArray *tz_lf_format_to_parts(TzListFormat *f, const TzArray *list, const TzIntlShape *shape) {
  const TzIcu *icu = tz_icu();
  TzUErrorCode status = 0;
  void *result = lf_result(f, list, &status);
  const void *value = icu->ulistfmt_resultAsValue(result, &status);
  TzArray *parts = tz_arr_new(shape->type, 0, 8);
  if (!TZ_U_FAILURE(status)) {
    tz_intl_push_value_parts(parts, shape, value, UFIELD_CATEGORY_LIST, lf_part_type, NULL, NULL);
  }
  icu->ulistfmt_closeResult(result);

  return parts;
}

TzIntl *tz_dn_create(const TzIntlOptions *o) {
  const char *method = "Intl.DisplayNames";
  const TzIcu *icu = tz_intl_icu();
  char requested[160], found[64];
  if (!tz_intl_resolve_locale(icu, o->has_locale, o->locale, requested, found)) {
    return NULL;
  }
  static const char *const types[] = {"language", "region",        "script", "currency",
                                      "calendar", "dateTimeField", NULL};
  static const char *const fallbacks[] = {"code", "none", NULL};
  static const char *const displays[] = {"dialect", "standard", NULL};
  char b[5][TZ_INTL_TEXT];
  int failed = 0;
  tz_intl_option(o, TZ_IO_LOCALE_MATCHER, matchers, b[0], method, &failed);
  const char *style = tz_intl_option(o, TZ_IO_STYLE, widths, b[1], method, &failed);
  const char *type = tz_intl_option(o, TZ_IO_TYPE, types, b[2], method, &failed);
  if (!failed && !type) {
    tz_panicf("internal error: Intl.DisplayNames without a type");
  }
  const char *fallback = tz_intl_option(o, TZ_IO_FALLBACK, fallbacks, b[3], method, &failed);
  const char *display = tz_intl_option(o, TZ_IO_LANGUAGE_DISPLAY, displays, b[4], method, &failed);
  if (failed) {
    return NULL;
  }
  style = style ? style : "long";
  fallback = fallback ? fallback : "code";
  display = display ? display : "dialect";
  TzDisplayNames *d =
      (TzDisplayNames *)tz_intl_alloc(sizeof(TzDisplayNames), TZ_INTL_DISPLAY_NAMES, &dn_layout);
  TzIntl *r = &d->base;
  tz_intl_tag(icu, found, r->locale, sizeof r->locale);
  tz_intl_copy(d->icu_locale, sizeof d->icu_locale, found);
  tz_intl_set_text(r, TZ_IO_STYLE, style);
  tz_intl_set_text(r, TZ_IO_TYPE, type);
  tz_intl_set_text(r, TZ_IO_FALLBACK, fallback);
  if (strcmp(type, "language") == 0) {
    tz_intl_set_text(r, TZ_IO_LANGUAGE_DISPLAY, display);
  }
  int contexts[3] = {strcmp(style, "long") == 0 ? UDISPCTX_LENGTH_FULL : UDISPCTX_LENGTH_SHORT,
                     strcmp(display, "dialect") == 0 ? UDISPCTX_DIALECT_NAMES
                                                     : UDISPCTX_STANDARD_NAMES,
                     strcmp(fallback, "code") == 0 ? UDISPCTX_SUBSTITUTE : UDISPCTX_NO_SUBSTITUTE};
  TzUErrorCode status = 0;
  d->names = icu->uldn_openForContext(found, contexts, 3, &status);
  if (TZ_U_FAILURE(status) || !d->names) {
    tz_intl_no_icu();
  }
  if (strcmp(type, "dateTimeField") == 0) {
    status = 0;
    d->generator = icu->udatpg_open(found, &status);
    if (TZ_U_FAILURE(status)) {
      tz_intl_no_icu();
    }
  }

  return r;
}

static void dn_drop(void *object) {
  TzDisplayNames *d = (TzDisplayNames *)object;
  const TzIcu *icu = tz_icu();
  if (d->names) {
    icu->uldn_close(d->names);
  }
  if (d->generator) {
    icu->udatpg_close(d->generator);
  }
  tz_obj_free(d);
}

static int all_of(const char *text, int (*test)(char)) {
  for (; *text; ++text) {
    if (!test(*text)) {
      return 0;
    }
  }

  return 1;
}

static int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int checked_code(const TzIcu *icu, const char *type, const char *code, char *id,
                        size_t size) {
  size_t n = strlen(code);
  if (strcmp(type, "region") == 0) {
    if (!((n == 2 && all_of(code, is_alpha)) || (n == 3 && all_of(code, is_digit)))) {
      return 0;
    }
  } else if (strcmp(type, "script") == 0) {
    if (n != 4 || !all_of(code, is_alpha)) {
      return 0;
    }
  } else if (strcmp(type, "currency") == 0) {
    if (n != 3 || !all_of(code, is_alpha)) {
      return 0;
    }
  } else if (strcmp(type, "calendar") == 0) {
    if (!tz_intl_is_keyword_type(code)) {
      return 0;
    }
    const char *legacy = icu->uloc_toLegacyType("ca", code);
    tz_intl_copy(id, size, legacy ? legacy : code);
    return 1;
  } else if (strcmp(type, "language") == 0) {
    char parsed_id[160];
    int32_t parsed = 0;
    TzUErrorCode status = 0;
    int32_t m =
        icu->uloc_forLanguageTag(code, parsed_id, (int32_t)sizeof parsed_id - 1, &parsed, &status);
    if (TZ_U_FAILURE(status) || m <= 0 || parsed != (int32_t)n || strchr(parsed_id, '@') ||
        !is_alpha(code[0]) || strncmp(code, "root", 4) == 0) {
      return 0;
    }
    parsed_id[m] = 0;
    status = 0;
    m = icu->uloc_canonicalize(parsed_id, id, (int32_t)size - 1, &status);
    if (TZ_U_FAILURE(status) || m <= 0) {
      return 0;
    }
    id[m] = 0;
    return 1;
  }
  tz_intl_copy(id, size, code);

  return 1;
}

static int date_field(const char *code) {
  static const struct {
    const char *name;
    int field;
  } fields[] = {{"era", 0},        {"year", 1},    {"quarter", 2}, {"month", 3},
                {"weekOfYear", 4}, {"weekday", 6}, {"day", 9},     {"dayPeriod", 10},
                {"hour", 11},      {"minute", 12}, {"second", 13}, {"timeZoneName", 15}};
  for (size_t i = 0; i < sizeof fields / sizeof fields[0]; ++i) {
    if (strcmp(fields[i].name, code) == 0) {
      return fields[i].field;
    }
  }

  return -1;
}

int32_t tz_dn_of(TzStr *out, TzDisplayNames *d, void *code_obj, uint64_t code_meta) {
  const TzIcu *icu = tz_icu();
  const char *type = d->base.text[TZ_IO_TYPE];
  TzStr given = {code_obj, code_meta};
  char code[160], id[160];
  *out = tz_str_from_ascii("", 0);
  int field = -1;
  if (!tz_intl_ascii(given, code, sizeof code) || !code[0] ||
      !checked_code(icu, type, code, id, sizeof id) ||
      (strcmp(type, "dateTimeField") == 0 && (field = date_field(code)) < 0)) {
    return 0;
  }
  TzUChar name[256];
  int32_t n = 0;
  TzUErrorCode status = 0;
  const void *names = d->names;
  if (strcmp(type, "language") == 0) {
    n = icu->uldn_localeDisplayName(names, id, name, 256, &status);
  } else if (strcmp(type, "region") == 0) {
    n = icu->uldn_regionDisplayName(names, id, name, 256, &status);
  } else if (strcmp(type, "script") == 0) {
    n = icu->uldn_scriptDisplayName(names, id, name, 256, &status);
  } else if (strcmp(type, "currency") == 0) {
    TzUChar units[4] = {(TzUChar)id[0], (TzUChar)id[1], (TzUChar)id[2], 0};
    const TzUChar *text =
        icu->ucurr_getName(units, d->icu_locale, UCURR_LONG_NAME, NULL, &n, &status);
    if (status == U_USING_DEFAULT_WARNING || !text || n >= 256) {
      status = U_ILLEGAL_ARGUMENT_ERROR;
    } else {
      memcpy(name, text, (size_t)n * sizeof(TzUChar));
    }
  } else if (strcmp(type, "calendar") == 0) {
    n = icu->uldn_keyValueDisplayName(names, "calendar", id, name, 256, &status);
  } else {
    n = icu->udatpg_getFieldDisplayName(d->generator, field, width_of(d->base.text[TZ_IO_STYLE]),
                                        name, 256, &status);
  }
  if (!TZ_U_FAILURE(status) && n > 0 && n < 256) {
    *out = tz_str_from_utf16(name, (size_t)n);
    return 1;
  }
  if (strcmp(d->base.text[TZ_IO_FALLBACK], "code") == 0) {
    tz_str_retain_value(given);
    *out = given;
    return 1;
  }

  return 0;
}
