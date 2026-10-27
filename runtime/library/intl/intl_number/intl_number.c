#include "runtime/library/intl/intl/intl.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/array/array.h"
#include "runtime/core/closure/closure.h"
#include "runtime/core/error/error.h"
#include "runtime/core/output/output.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include "runtime/library/decimal/decimal/decimal.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

enum {
  UNUM_INTEGER_FIELD = 0,
  UNUM_FRACTION_FIELD = 1,
  UNUM_DECIMAL_SEPARATOR_FIELD = 2,
  UNUM_EXPONENT_SYMBOL_FIELD = 3,
  UNUM_EXPONENT_SIGN_FIELD = 4,
  UNUM_EXPONENT_FIELD = 5,
  UNUM_GROUPING_SEPARATOR_FIELD = 6,
  UNUM_CURRENCY_FIELD = 7,
  UNUM_PERCENT_FIELD = 8,
  UNUM_PERMILL_FIELD = 9,
  UNUM_SIGN_FIELD = 10,
  UNUM_MEASURE_UNIT_FIELD = 11,
  UNUM_COMPACT_FIELD = 12,
  UNUM_APPROXIMATELY_SIGN_FIELD = 13,
  UFIELD_CATEGORY_NUMBER = 2,
};

typedef struct {
  TzIntl base;
  void *formatter;
} TzNumberFormat;

typedef struct {
  TzIntl base;
  void *rules;
  void *formatter;
} TzPluralRules;

static void nf_drop(void *object);
static void pr_drop(void *object);
static const TzLayout nf_layout = {nf_drop, NULL, 0, TZ_LAYOUT_INTL, "NumberFormat", 12, 0};
static const TzLayout pr_layout = {pr_drop, NULL, 0, TZ_LAYOUT_INTL, "PluralRules", 11, 0};

static const char *const matchers[] = {"lookup", "best fit", NULL};

static void put(char *skeleton, size_t size, const char *stem) {
  size_t n = strlen(skeleton), m = strlen(stem);
  if (n + m + 2 > size) {
    return;
  }
  if (n) {
    skeleton[n++] = ' ';
  }
  memcpy(skeleton + n, stem, m + 1);
}

static void repeat(char *out, size_t *n, char c, int count) {
  for (int i = 0; i < count; ++i) {
    out[(*n)++] = c;
  }
  out[*n] = 0;
}

static int has_number(const TzIntlOptions *o, int index) {
  return (o->has_number & (UINT64_C(1) << index)) != 0;
}

static int has_text(const TzIntlOptions *o, int index) {
  return (o->has_text & (UINT64_C(1) << index)) != 0;
}

static int default_number(const TzIntlOptions *o, int index, int min, int max, int fallback,
                          int *failed) {
  return tz_intl_number_option(o, index, min, max, fallback, failed);
}

static const char *rounding_stem(const char *mode) {
  static const char *const modes[][2] = {
      {"ceil", "rounding-mode-ceiling"},
      {"floor", "rounding-mode-floor"},
      {"expand", "rounding-mode-up"},
      {"trunc", "rounding-mode-down"},
      {"halfCeil", "rounding-mode-half-ceiling"},
      {"halfFloor", "rounding-mode-half-floor"},
      {"halfExpand", "rounding-mode-half-up"},
      {"halfTrunc", "rounding-mode-half-down"},
      {"halfEven", "rounding-mode-half-even"},
  };
  for (size_t i = 0; i < sizeof modes / sizeof modes[0]; ++i) {
    if (strcmp(modes[i][0], mode) == 0) {
      return modes[i][1];
    }
  }

  return "rounding-mode-half-up";
}

int tz_nf_digit_options(const TzIntlOptions *o, TzIntl *r, int mnfd_default, int mxfd_default,
                        int compact, const char *method, char *skeleton, size_t size) {
  static const char *const modes[] = {"ceil",     "floor",     "expand",     "trunc",
                                      "halfCeil", "halfFloor", "halfExpand", "halfTrunc",
                                      "halfEven", NULL};
  static const char *const priorities[] = {"auto", "morePrecision", "lessPrecision", NULL};
  static const char *const trailings[] = {"auto", "stripIfInteger", NULL};
  static const int increments[] = {1,   2,   5,   10,   20,   25,   50,  100,
                                   200, 250, 500, 1000, 2000, 2500, 5000};
  int failed = 0;
  int mnid = tz_intl_number_option(o, TZ_IO_MINIMUM_INTEGER_DIGITS, 1, 21, 1, &failed);
  int increment = tz_intl_number_option(o, TZ_IO_ROUNDING_INCREMENT, 1, 5000, 1, &failed);
  if (failed) {
    return 0;
  }
  int valid = 0;
  for (size_t i = 0; i < sizeof increments / sizeof increments[0]; ++i) {
    valid |= increments[i] == increment;
  }
  if (!valid) {
    tz_throw(TZ_RANGE_ERROR, "roundingIncrement value is out of range.");
    return 0;
  }
  char b1[TZ_INTL_TEXT], b2[TZ_INTL_TEXT], b3[TZ_INTL_TEXT];
  const char *mode = tz_intl_option(o, TZ_IO_ROUNDING_MODE, modes, b1, method, &failed);
  const char *priority =
      tz_intl_option(o, TZ_IO_ROUNDING_PRIORITY, priorities, b2, method, &failed);
  const char *trailing =
      tz_intl_option(o, TZ_IO_TRAILING_ZERO_DISPLAY, trailings, b3, method, &failed);
  if (failed) {
    return 0;
  }
  mode = mode ? mode : "halfExpand";
  priority = priority ? priority : "auto";
  trailing = trailing ? trailing : "auto";

  int has_mnfd = has_number(o, TZ_IO_MINIMUM_FRACTION_DIGITS);
  int has_mxfd = has_number(o, TZ_IO_MAXIMUM_FRACTION_DIGITS);
  int has_sd = has_number(o, TZ_IO_MINIMUM_SIGNIFICANT_DIGITS) ||
               has_number(o, TZ_IO_MAXIMUM_SIGNIFICANT_DIGITS);
  int has_fd = has_mnfd || has_mxfd;
  int need_sd = 1, need_fd = 1, is_auto = strcmp(priority, "auto") == 0;
  if (is_auto) {
    need_sd = has_sd;
    if (need_sd || (!has_fd && compact)) {
      need_fd = 0;
    }
  }
  int mnsd = 0, mxsd = 0, mnfd = 0, mxfd = 0;
  if (need_sd) {
    if (has_sd) {
      mnsd = default_number(o, TZ_IO_MINIMUM_SIGNIFICANT_DIGITS, 1, 21, 1, &failed);
      if (failed) {
        return 0;
      }
      mxsd = default_number(o, TZ_IO_MAXIMUM_SIGNIFICANT_DIGITS, 1, 21, 21, &failed);
      mxsd = mxsd < mnsd ? mnsd : mxsd;
    } else {
      mnsd = 1;
      mxsd = 21;
    }
  }
  if (need_fd && !failed) {
    if (has_fd) {
      mnfd = default_number(o, TZ_IO_MINIMUM_FRACTION_DIGITS, 0, 100, -1, &failed);
      if (!failed) {
        mxfd = default_number(o, TZ_IO_MAXIMUM_FRACTION_DIGITS, 0, 100, -1, &failed);
      }
      if (failed) {
        return 0;
      }
      if (!has_mnfd) {
        mnfd = mnfd_default < mxfd ? mnfd_default : mxfd;
      } else if (!has_mxfd) {
        mxfd = mxfd_default > mnfd ? mxfd_default : mnfd;
      } else if (mnfd > mxfd) {
        mxfd = mnfd;
      }
    } else {
      mnfd = mnfd_default;
      mxfd = mxfd_default;
    }
  }
  if (failed) {
    return 0;
  }
  enum { FRACTION, SIGNIFICANT, MORE, LESS } type;
  if (!need_sd && !need_fd) {
    mnfd = mxfd = 0;
    mnsd = 1;
    mxsd = 2;
    type = MORE;
    priority = "morePrecision";
  } else if (is_auto) {
    type = need_sd ? SIGNIFICANT : FRACTION;
  } else {
    type = strcmp(priority, "morePrecision") == 0 ? MORE : LESS;
  }
  if (increment != 1 && type != FRACTION) {
    increment = 1;
  } else if (increment != 1) {
    mxfd = mnfd;
  }

  r->number[TZ_IO_MINIMUM_INTEGER_DIGITS] = mnid;
  if (type != SIGNIFICANT) {
    r->number[TZ_IO_MINIMUM_FRACTION_DIGITS] = mnfd;
    r->number[TZ_IO_MAXIMUM_FRACTION_DIGITS] = mxfd;
  }
  if (type != FRACTION) {
    r->number[TZ_IO_MINIMUM_SIGNIFICANT_DIGITS] = mnsd;
    r->number[TZ_IO_MAXIMUM_SIGNIFICANT_DIGITS] = mxsd;
  }
  r->number[TZ_IO_ROUNDING_INCREMENT] = increment;
  tz_intl_set_text(r, TZ_IO_ROUNDING_MODE, mode);
  tz_intl_set_text(r, TZ_IO_ROUNDING_PRIORITY, priority);
  tz_intl_set_text(r, TZ_IO_TRAILING_ZERO_DISPLAY, trailing);

  char stem[256];
  size_t n = 0;
  if (mnid > 1) {
    strcpy(stem, "integer-width/*");
    n = strlen(stem);
    repeat(stem, &n, '0', mnid);
    put(skeleton, size, stem);
  }
  char fraction[128], significant[32];
  size_t f = 0, s = 0;
  if (mxfd == 0) {
    strcpy(fraction, "precision-integer");
  } else {
    fraction[f++] = '.';
    repeat(fraction, &f, '0', mnfd);
    repeat(fraction, &f, '#', mxfd - mnfd);
  }
  repeat(significant, &s, '@', mnsd);
  repeat(significant, &s, '#', mxsd - mnsd);
  if (increment != 1) {
    char digits[16];
    int k = snprintf(digits, sizeof digits, "%d", increment);
    strcpy(stem, "precision-increment/");
    n = strlen(stem);
    if (mxfd == 0) {
      memcpy(stem + n, digits, (size_t)k + 1);
    } else {
      int whole = k - mxfd;
      if (whole <= 0) {
        stem[n++] = '0';
        stem[n++] = '.';
        repeat(stem, &n, '0', -whole);
        memcpy(stem + n, digits, (size_t)k + 1);
      } else {
        memcpy(stem + n, digits, (size_t)whole);
        n += (size_t)whole;
        stem[n++] = '.';
        memcpy(stem + n, digits + whole, (size_t)(k - whole) + 1);
      }
    }
  } else if (type == FRACTION) {
    strcpy(stem, fraction);
  } else if (type == SIGNIFICANT) {
    strcpy(stem, significant);
  } else {
    snprintf(stem, sizeof stem, "%s/%s%c", fraction, significant, type == MORE ? 'r' : 's');
  }
  if (strcmp(trailing, "stripIfInteger") == 0) {
    strcat(stem, "/w");
  }
  put(skeleton, size, stem);
  put(skeleton, size, rounding_stem(mode));

  return 1;
}

static int is_sanctioned(const char *unit, size_t length) {
  static const char *const units[] = {
      "acre",        "bit",         "byte",        "celsius",
      "centimeter",  "day",         "degree",      "fahrenheit",
      "fluid-ounce", "foot",        "gallon",      "gigabit",
      "gigabyte",    "gram",        "hectare",     "hour",
      "inch",        "kilobit",     "kilobyte",    "kilogram",
      "kilometer",   "liter",       "megabit",     "megabyte",
      "meter",       "microsecond", "mile",        "mile-scandinavian",
      "milliliter",  "millimeter",  "millisecond", "minute",
      "month",       "nanosecond",  "ounce",       "percent",
      "petabyte",    "pound",       "second",      "stone",
      "terabit",     "terabyte",    "week",        "yard",
      "year"};
  for (size_t i = 0; i < sizeof units / sizeof units[0]; ++i) {
    if (strlen(units[i]) == length && strncmp(units[i], unit, length) == 0) {
      return 1;
    }
  }

  return 0;
}

static int is_well_formed_unit(const char *unit) {
  size_t length = strlen(unit);
  if (is_sanctioned(unit, length)) {
    return 1;
  }
  const char *per = strstr(unit, "-per-");

  return per && is_sanctioned(unit, (size_t)(per - unit)) &&
         is_sanctioned(per + 5, strlen(per + 5));
}

static int is_well_formed_currency(const char *code) {
  if (strlen(code) != 3) {
    return 0;
  }
  for (int i = 0; i < 3; ++i) {
    char c = code[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
      return 0;
    }
  }

  return 1;
}

static int currency_digits(const TzIcu *icu, const char *code) {
  TzUChar units[4] = {(TzUChar)code[0], (TzUChar)code[1], (TzUChar)code[2], 0};
  TzUErrorCode status = 0;
  int32_t digits = icu->ucurr_getDefaultFractionDigits(units, &status);

  return TZ_U_FAILURE(status) || digits < 0 ? 2 : digits;
}

static void *open_formatter(const TzIcu *icu, const char *skeleton, const char *locale) {
  TzUChar units[512];
  int32_t n = 0;
  for (; skeleton[n] && n < 511; ++n) {
    units[n] = (unsigned char)skeleton[n];
  }
  units[n] = 0;
  char parse_error[128];
  TzUErrorCode status = 0;
  void *formatter =
      icu->unumf_openForSkeletonAndLocaleWithError(units, n, locale, parse_error, &status);
  if (TZ_U_FAILURE(status) || !formatter) {
    tz_panicf("the ICU of the system cannot format numbers as '%s'", skeleton);
  }

  return formatter;
}

static void with_numbering(const TzIcu *icu, const char *found, const char *system, char *id,
                           size_t size) {
  tz_intl_copy(id, size, found);
  if (!system) {
    return;
  }
  TzUErrorCode status = 0;
  icu->uloc_setKeywordValue("numbers", system, id, (int32_t)size, &status);
  if (TZ_U_FAILURE(status)) {
    tz_intl_copy(id, size, found);
  }
}

static void numbering_of(const TzIcu *icu, const char *id, TzIntl *r) {
  TzUErrorCode status = 0;
  void *system = icu->unumsys_open(id, &status);
  tz_intl_set_text(r, TZ_IO_NUMBERING_SYSTEM,
                   !TZ_U_FAILURE(status) && system ? icu->unumsys_getName(system) : "latn");
  if (system) {
    icu->unumsys_close(system);
  }
}

static const char *choose_numbering(const TzIcu *icu, const char *requested, const char *option,
                                    char *keyword, size_t size, int *in_tag) {
  tz_intl_keyword(icu, requested, "nu", keyword, size);
  int keyword_ok = keyword[0] && tz_intl_has_numbering(icu, keyword);
  *in_tag = 0;
  if (option && tz_intl_has_numbering(icu, option)) {
    *in_tag = keyword_ok && strcmp(keyword, option) == 0;
    return option;
  }
  if (keyword_ok) {
    *in_tag = 1;
    return keyword;
  }

  return NULL;
}

TzIntl *tz_nf_create(const TzIntlOptions *o, const char *method) {
  const TzIcu *icu = tz_intl_icu();
  char requested[160], found[64];
  if (!tz_intl_resolve_locale(icu, o->has_locale, o->locale, requested, found)) {
    return NULL;
  }
  static const char *const styles[] = {"decimal", "percent", "currency", "unit", NULL};
  static const char *const currency_displays[] = {"code", "symbol", "narrowSymbol", "name", NULL};
  static const char *const currency_signs[] = {"standard", "accounting", NULL};
  static const char *const unit_displays[] = {"short", "narrow", "long", NULL};
  static const char *const notations[] = {"standard", "scientific", "engineering", "compact", NULL};
  static const char *const compact_displays[] = {"short", "long", NULL};
  static const char *const groupings[] = {"min2", "auto", "always", "true", "false", NULL};
  static const char *const signs[] = {"auto", "never", "always", "exceptZero", "negative", NULL};
  char b[12][TZ_INTL_TEXT];
  int failed = 0;
  tz_intl_option(o, TZ_IO_LOCALE_MATCHER, matchers, b[0], method, &failed);
  const char *numbering = tz_intl_option(o, TZ_IO_NUMBERING_SYSTEM, NULL, b[1], method, &failed);
  if (failed) {
    return NULL;
  }
  if (numbering && !tz_intl_is_keyword_type(numbering)) {
    tz_throw(TZ_RANGE_ERROR, "Invalid numberingSystem : %s", numbering);
    return NULL;
  }
  TzNumberFormat *nf =
      (TzNumberFormat *)tz_intl_alloc(sizeof(TzNumberFormat), TZ_INTL_NUMBER_FORMAT, &nf_layout);
  TzIntl *r = &nf->base;
  char keyword[TZ_INTL_TEXT], id[160], tag[TZ_INTL_LOCALE];
  int in_tag;
  const char *system =
      choose_numbering(icu, requested, numbering, keyword, sizeof keyword, &in_tag);
  tz_intl_tag(icu, found, tag, sizeof tag);
  if (in_tag && strlen(tag) + strlen(system) + 7 < sizeof tag) {
    strcat(tag, "-u-nu-");
    strcat(tag, system);
  }
  tz_intl_copy(r->locale, sizeof r->locale, tag);
  with_numbering(icu, found, system, id, sizeof id);
  numbering_of(icu, id, r);

  const char *style = tz_intl_option(o, TZ_IO_STYLE, styles, b[2], method, &failed);
  const char *currency = tz_intl_option(o, TZ_IO_CURRENCY, NULL, b[3], method, &failed);
  style = style ? style : "decimal";
  if (!failed && !currency && strcmp(style, "currency") == 0) {
    tz_throw(TZ_TYPE_ERROR, "Currency code is required with currency style.");
    failed = 1;
  } else if (!failed && currency && !is_well_formed_currency(currency)) {
    tz_throw(TZ_RANGE_ERROR, "Invalid currency code : %s", currency);
    failed = 1;
  }
  const char *currency_display =
      tz_intl_option(o, TZ_IO_CURRENCY_DISPLAY, currency_displays, b[4], method, &failed);
  const char *currency_sign =
      tz_intl_option(o, TZ_IO_CURRENCY_SIGN, currency_signs, b[5], method, &failed);
  const char *unit = tz_intl_option(o, TZ_IO_UNIT, NULL, b[6], method, &failed);
  if (!failed && !unit && strcmp(style, "unit") == 0) {
    tz_throw(TZ_TYPE_ERROR, "Invalid unit argument for Intl.NumberFormat() ''");
    failed = 1;
  } else if (!failed && unit && !is_well_formed_unit(unit)) {
    tz_throw(TZ_RANGE_ERROR, "Invalid unit argument for Intl.NumberFormat() '%s'", unit);
    failed = 1;
  }
  const char *unit_display =
      tz_intl_option(o, TZ_IO_UNIT_DISPLAY, unit_displays, b[7], method, &failed);
  if (failed) {
    nf_drop(nf);
    return NULL;
  }
  currency_display = currency_display ? currency_display : "symbol";
  currency_sign = currency_sign ? currency_sign : "standard";
  unit_display = unit_display ? unit_display : "short";
  char skeleton[512] = "", stem[TZ_INTL_TEXT + 32];
  tz_intl_set_text(r, TZ_IO_STYLE, style);
  int mnfd_default = 0, mxfd_default = 3;
  if (strcmp(style, "currency") == 0) {
    char code[4] = {0};
    for (int i = 0; i < 3; ++i) {
      code[i] = (char)(currency[i] >= 'a' ? currency[i] - 32 : currency[i]);
    }
    tz_intl_set_text(r, TZ_IO_CURRENCY, code);
    tz_intl_set_text(r, TZ_IO_CURRENCY_DISPLAY, currency_display);
    tz_intl_set_text(r, TZ_IO_CURRENCY_SIGN, currency_sign);
    snprintf(stem, sizeof stem, "currency/%s", code);
    put(skeleton, sizeof skeleton, stem);
    if (strcmp(currency_display, "code") == 0) {
      put(skeleton, sizeof skeleton, "unit-width-iso-code");
    } else if (strcmp(currency_display, "name") == 0) {
      put(skeleton, sizeof skeleton, "unit-width-full-name");
    } else if (strcmp(currency_display, "narrowSymbol") == 0) {
      put(skeleton, sizeof skeleton, "unit-width-narrow");
    }
    mnfd_default = mxfd_default = currency_digits(icu, code);
  } else if (strcmp(style, "unit") == 0) {
    tz_intl_set_text(r, TZ_IO_UNIT, unit);
    tz_intl_set_text(r, TZ_IO_UNIT_DISPLAY, unit_display);
    snprintf(stem, sizeof stem, "unit/%s", unit);
    put(skeleton, sizeof skeleton, stem);
    put(skeleton, sizeof skeleton,
        strcmp(unit_display, "long") == 0     ? "unit-width-full-name"
        : strcmp(unit_display, "narrow") == 0 ? "unit-width-narrow"
                                              : "unit-width-short");
  } else if (strcmp(style, "percent") == 0) {
    put(skeleton, sizeof skeleton, "percent scale/100");
    mxfd_default = 0;
  }

  const char *notation = tz_intl_option(o, TZ_IO_NOTATION, notations, b[8], method, &failed);
  notation = notation ? notation : "standard";
  int compact = strcmp(notation, "compact") == 0;
  if (failed || !tz_nf_digit_options(o, r, mnfd_default, mxfd_default, compact, method, skeleton,
                                     sizeof skeleton)) {
    nf_drop(nf);
    return NULL;
  }
  const char *compact_display =
      tz_intl_option(o, TZ_IO_COMPACT_DISPLAY, compact_displays, b[9], method, &failed);
  compact_display = compact_display ? compact_display : "short";
  tz_intl_set_text(r, TZ_IO_NOTATION, notation);
  if (compact) {
    tz_intl_set_text(r, TZ_IO_COMPACT_DISPLAY, compact_display);
    put(skeleton, sizeof skeleton,
        strcmp(compact_display, "long") == 0 ? "compact-long" : "compact-short");
  } else if (strcmp(notation, "scientific") == 0) {
    put(skeleton, sizeof skeleton, "scientific");
  } else if (strcmp(notation, "engineering") == 0) {
    put(skeleton, sizeof skeleton, "engineering");
  }

  const char *grouping = compact ? "min2" : "auto";
  int flag = tz_intl_bool_option(o, TZ_IO_USE_GROUPING);
  if (flag >= 0) {
    grouping = flag ? "always" : "false";
  } else if (has_text(o, TZ_IO_USE_GROUPING) && !failed) {
    char text[TZ_INTL_TEXT];
    int ascii = tz_intl_ascii(o->text[TZ_IO_USE_GROUPING], text, sizeof text);
    if (ascii && !text[0]) {
      grouping = "false";
    } else {
      const char *value = tz_intl_option(o, TZ_IO_USE_GROUPING, groupings, b[10], method, &failed);
      if (value && strcmp(value, "true") != 0 && strcmp(value, "false") != 0) {
        grouping = value;
      }
    }
  }
  const char *sign = tz_intl_option(o, TZ_IO_SIGN_DISPLAY, signs, b[11], method, &failed);
  if (failed) {
    nf_drop(nf);
    return NULL;
  }
  sign = sign ? sign : "auto";
  tz_intl_set_text(r, TZ_IO_USE_GROUPING, grouping);
  tz_intl_set_text(r, TZ_IO_SIGN_DISPLAY, sign);
  put(skeleton, sizeof skeleton,
      strcmp(grouping, "false") == 0    ? "group-off"
      : strcmp(grouping, "min2") == 0   ? "group-min2"
      : strcmp(grouping, "always") == 0 ? "group-on-aligned"
                                        : "group-auto");
  int accounting = strcmp(r->text[TZ_IO_CURRENCY_SIGN], "accounting") == 0;
  const char *sign_stem = accounting ? "sign-accounting" : "sign-auto";
  if (strcmp(sign, "never") == 0) {
    sign_stem = "sign-never";
  } else if (strcmp(sign, "always") == 0) {
    sign_stem = accounting ? "sign-accounting-always" : "sign-always";
  } else if (strcmp(sign, "exceptZero") == 0) {
    sign_stem = accounting ? "sign-accounting-except-zero" : "sign-except-zero";
  } else if (strcmp(sign, "negative") == 0) {
    sign_stem = accounting ? "sign-accounting-negative" : "sign-negative";
  }
  put(skeleton, sizeof skeleton, sign_stem);
  nf->formatter = open_formatter(icu, skeleton, id);

  return r;
}

static void nf_drop(void *object) {
  TzNumberFormat *nf = (TzNumberFormat *)object;
  if (nf->formatter) {
    tz_icu()->unumf_close(nf->formatter);
  }
  tz_obj_free(nf);
}

void tz_nf_format_into(const void *formatter, const TzIntlNumber *n, void *result,
                       TzUErrorCode *status) {
  const TzIcu *icu = tz_icu();
  if (n->kind == TZ_IN_INTEGER) {
    icu->unumf_formatInt(formatter, n->integer, result, status);
  } else if (n->kind == TZ_IN_DECIMAL) {
    TzDec value = {n->integer, n->meta};
    TzBuf text;
    tz_buf_init(&text);
    tz_buf_decimal(&text, value);
    char local[128],
        *digits = text.length < sizeof local ? local : (char *)tz_alloc(text.length + 1);
    for (size_t i = 0; i < text.length; ++i) {
      digits[i] = (char)text.data[i];
    }
    digits[text.length] = 0;
    icu->unumf_formatDecimal(formatter, digits, (int32_t)text.length, result, status);
    if (digits != local) {
      tz_free(digits);
    }
    tz_buf_free(&text);
  } else {
    icu->unumf_formatDouble(formatter, n->d, result, status);
  }
}

static TzStr format_string(const void *formatter, const TzIntlNumber *n) {
  const TzIcu *icu = tz_icu();
  TzUErrorCode status = 0;
  void *result = icu->unumf_openResult(&status);
  tz_nf_format_into(formatter, n, result, &status);
  const void *value = icu->unumf_resultAsValue(result, &status);
  int32_t length = 0;
  const TzUChar *text =
      TZ_U_FAILURE(status) ? NULL : icu->ufmtval_getString(value, &length, &status);
  TzStr s = text && !TZ_U_FAILURE(status) ? tz_str_from_utf16(text, (size_t)length)
                                          : tz_str_from_ascii("", 0);
  icu->unumf_closeResult(result);

  return s;
}

void tz_nf_format(TzStr *out, TzNumberFormat *nf, int32_t kind, double d, int64_t integer,
                  uint64_t meta) {
  TzIntlNumber n = {kind, d, integer, meta};
  *out = format_string(nf->formatter, &n);
}

typedef struct {
  int nan, infinite, negative;
} NumberKind;

static const char *number_part_type(int32_t field, void *ctx) {
  const NumberKind *k = (const NumberKind *)ctx;
  switch (field) {
  case UNUM_INTEGER_FIELD:
    return k->nan ? "nan" : k->infinite ? "infinity" : "integer";
  case UNUM_FRACTION_FIELD:
    return "fraction";
  case UNUM_DECIMAL_SEPARATOR_FIELD:
    return "decimal";
  case UNUM_EXPONENT_SYMBOL_FIELD:
    return "exponentSeparator";
  case UNUM_EXPONENT_SIGN_FIELD:
    return "exponentMinusSign";
  case UNUM_EXPONENT_FIELD:
    return "exponentInteger";
  case UNUM_GROUPING_SEPARATOR_FIELD:
    return "group";
  case UNUM_CURRENCY_FIELD:
    return "currency";
  case UNUM_PERCENT_FIELD:
  case UNUM_PERMILL_FIELD:
    return "percentSign";
  case UNUM_SIGN_FIELD:
    return k->negative ? "minusSign" : "plusSign";
  case UNUM_MEASURE_UNIT_FIELD:
    return "unit";
  case UNUM_COMPACT_FIELD:
    return "compact";
  case UNUM_APPROXIMATELY_SIGN_FIELD:
    return "approximatelySign";
  default:
    return "literal";
  }
}

static NumberKind kind_of(const TzIntlNumber *n) {
  NumberKind k = {0, 0, 0};
  if (n->kind == TZ_IN_INTEGER) {
    k.negative = n->integer < 0;
  } else if (n->kind == TZ_IN_DECIMAL) {
    TzDec value = {n->integer, n->meta};
    TzBuf text;
    tz_buf_init(&text);
    tz_buf_decimal(&text, value);
    k.negative = text.length > 0 && text.data[0] == '-';
    tz_buf_free(&text);
  } else {
    k.nan = isnan(n->d);
    k.infinite = isinf(n->d);
    k.negative = signbit(n->d) != 0;
  }

  return k;
}

TzArray *tz_nf_format_to_parts(TzNumberFormat *nf, int32_t kind, double d, int64_t integer,
                               uint64_t meta, const TzIntlShape *shape) {
  const TzIcu *icu = tz_icu();
  TzIntlNumber n = {kind, d, integer, meta};
  NumberKind k = kind_of(&n);
  TzUErrorCode status = 0;
  void *result = icu->unumf_openResult(&status);
  tz_nf_format_into(nf->formatter, &n, result, &status);
  const void *value = icu->unumf_resultAsValue(result, &status);
  TzArray *parts = tz_arr_new(shape->type, 0, 8);
  if (!TZ_U_FAILURE(status)) {
    tz_intl_push_value_parts(parts, shape, value, UFIELD_CATEGORY_NUMBER, number_part_type, &k,
                             NULL);
  }
  icu->unumf_closeResult(result);

  return parts;
}

void tz_num_locale(TzStr *out, int32_t kind, double d, int64_t integer, uint64_t meta,
                   const TzIntlOptions *o) {
  TzIntl *nf = tz_intl_cached(TZ_INTL_NUMBER_FORMAT, o, "Number.prototype.toLocaleString");
  if (!nf) {
    *out = tz_str_from_ascii("", 0);
    return;
  }
  TzIntlNumber n = {kind, d, integer, meta};
  *out = format_string(((TzNumberFormat *)nf)->formatter, &n);
}

TzIntl *tz_pr_create(const TzIntlOptions *o) {
  const char *method = "Intl.PluralRules";
  const TzIcu *icu = tz_intl_icu();
  char requested[160], found[64];
  if (!tz_intl_resolve_locale(icu, o->has_locale, o->locale, requested, found)) {
    return NULL;
  }
  static const char *const types[] = {"cardinal", "ordinal", NULL};
  char b[2][TZ_INTL_TEXT];
  int failed = 0;
  tz_intl_option(o, TZ_IO_LOCALE_MATCHER, matchers, b[0], method, &failed);
  const char *type = tz_intl_option(o, TZ_IO_TYPE, types, b[1], method, &failed);
  if (failed) {
    return NULL;
  }
  type = type ? type : "cardinal";
  TzPluralRules *pr =
      (TzPluralRules *)tz_intl_alloc(sizeof(TzPluralRules), TZ_INTL_PLURAL_RULES, &pr_layout);
  TzIntl *r = &pr->base;
  tz_intl_tag(icu, found, r->locale, sizeof r->locale);
  tz_intl_set_text(r, TZ_IO_TYPE, type);
  char skeleton[512] = "";
  if (!tz_nf_digit_options(o, r, 0, 3, 0, method, skeleton, sizeof skeleton)) {
    pr_drop(pr);
    return NULL;
  }
  TzUErrorCode status = 0;
  pr->rules = icu->uplrules_openForType(found, strcmp(type, "ordinal") == 0, &status);
  if (TZ_U_FAILURE(status) || !pr->rules) {
    tz_intl_no_icu();
  }
  pr->formatter = open_formatter(icu, skeleton, found);

  return r;
}

static void pr_drop(void *object) {
  TzPluralRules *pr = (TzPluralRules *)object;
  const TzIcu *icu = tz_icu();
  if (pr->rules) {
    icu->uplrules_close(pr->rules);
  }
  if (pr->formatter) {
    icu->unumf_close(pr->formatter);
  }
  tz_obj_free(pr);
}

void tz_pr_select(TzStr *out, TzPluralRules *pr, int32_t kind, double d, int64_t integer,
                  uint64_t meta) {
  const TzIcu *icu = tz_icu();
  TzIntlNumber n = {kind, d, integer, meta};
  TzUErrorCode status = 0;
  void *result = icu->unumf_openResult(&status);
  tz_nf_format_into(pr->formatter, &n, result, &status);
  TzUChar keyword[32];
  int32_t length = TZ_U_FAILURE(status)
                       ? 0
                       : icu->uplrules_selectFormatted(pr->rules, result, keyword, 32, &status);
  icu->unumf_closeResult(result);
  if (TZ_U_FAILURE(status) || length <= 0 || length >= 32) {
    *out = tz_str_from_ascii("other", 5);
  } else {
    *out = tz_str_from_utf16(keyword, (size_t)length);
  }
}

TzArray *tz_pr_categories(TzPluralRules *pr) {
  static const char *const order[] = {"zero", "one", "two", "few", "many", "other"};
  const TzIcu *icu = tz_icu();
  TzUErrorCode status = 0;
  void *keywords = icu->uplrules_getKeywords(pr->rules, &status);
  int present[6] = {0};
  if (!TZ_U_FAILURE(status) && keywords) {
    int32_t length;
    const char *keyword;
    while ((keyword = icu->uenum_next(keywords, &length, &status)) != NULL) {
      for (int i = 0; i < 6; ++i) {
        if (strcmp(order[i], keyword) == 0) {
          present[i] = 1;
        }
      }
    }
    icu->uenum_close(keywords);
  }
  TzArray *out = tz_arr_new(&tz_string_type, 0, 6);
  for (int i = 0; i < 6; ++i) {
    if (present[i]) {
      TzStr s = tz_str_from_ascii(order[i], strlen(order[i]));
      tz_arr_push_value(out, &s);
    }
  }

  return out;
}
