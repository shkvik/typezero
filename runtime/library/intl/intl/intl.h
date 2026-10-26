#ifndef TZ_INTL_H
#define TZ_INTL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  TZ_INTL_NUMBER_FORMAT = 1,
  TZ_INTL_COLLATOR = 2,
  TZ_INTL_PLURAL_RULES = 3,
  TZ_INTL_RELATIVE_TIME_FORMAT = 4,
  TZ_INTL_LIST_FORMAT = 5,
  TZ_INTL_DISPLAY_NAMES = 6,
  TZ_INTL_SEGMENTER = 7,
  TZ_INTL_SEGMENTS = 8,
};

#define TZ_INTL_OPTIONS(X)                                                                         \
  X(LOCALE_MATCHER, "localeMatcher")                                                               \
  X(NUMBERING_SYSTEM, "numberingSystem")                                                           \
  X(STYLE, "style")                                                                                \
  X(CURRENCY, "currency")                                                                          \
  X(CURRENCY_DISPLAY, "currencyDisplay")                                                           \
  X(CURRENCY_SIGN, "currencySign")                                                                 \
  X(UNIT, "unit")                                                                                  \
  X(UNIT_DISPLAY, "unitDisplay")                                                                   \
  X(MINIMUM_INTEGER_DIGITS, "minimumIntegerDigits")                                                \
  X(MINIMUM_FRACTION_DIGITS, "minimumFractionDigits")                                              \
  X(MAXIMUM_FRACTION_DIGITS, "maximumFractionDigits")                                              \
  X(MINIMUM_SIGNIFICANT_DIGITS, "minimumSignificantDigits")                                        \
  X(MAXIMUM_SIGNIFICANT_DIGITS, "maximumSignificantDigits")                                        \
  X(USE_GROUPING, "useGrouping")                                                                   \
  X(NOTATION, "notation")                                                                          \
  X(COMPACT_DISPLAY, "compactDisplay")                                                             \
  X(SIGN_DISPLAY, "signDisplay")                                                                   \
  X(ROUNDING_INCREMENT, "roundingIncrement")                                                       \
  X(ROUNDING_MODE, "roundingMode")                                                                 \
  X(ROUNDING_PRIORITY, "roundingPriority")                                                         \
  X(TRAILING_ZERO_DISPLAY, "trailingZeroDisplay")                                                  \
  X(USAGE, "usage")                                                                                \
  X(SENSITIVITY, "sensitivity")                                                                    \
  X(IGNORE_PUNCTUATION, "ignorePunctuation")                                                       \
  X(COLLATION, "collation")                                                                        \
  X(NUMERIC, "numeric")                                                                            \
  X(CASE_FIRST, "caseFirst")                                                                       \
  X(TYPE, "type")                                                                                  \
  X(FALLBACK, "fallback")                                                                          \
  X(LANGUAGE_DISPLAY, "languageDisplay")                                                           \
  X(GRANULARITY, "granularity")

enum {
#define TZ_INTL_OPTION_ENUM(id, name) TZ_IO_##id,
  TZ_INTL_OPTIONS(TZ_INTL_OPTION_ENUM)
#undef TZ_INTL_OPTION_ENUM
      TZ_IO_COUNT
};

enum {
  TZ_IF_TYPE,
  TZ_IF_VALUE,
  TZ_IF_UNIT,
  TZ_IF_SEGMENT,
  TZ_IF_INDEX,
  TZ_IF_INPUT,
  TZ_IF_WORD_LIKE,
  TZ_IF_COUNT
};

enum { TZ_IN_DOUBLE = 0, TZ_IN_INTEGER = 1, TZ_IN_DECIMAL = 2 };

#ifdef __cplusplus
}
#endif

#ifndef __cplusplus

#include "runtime/library/intl/icu/icu.h"
#include "runtime/core/array/array.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"

typedef struct {
  TzStr locale;
  TzStr text[TZ_IO_COUNT];
  double number[TZ_IO_COUNT];
  uint64_t has_text, has_number;
  int32_t has_locale;
} TzIntlOptions;

typedef struct {
  int32_t kind;
  double d;
  int64_t integer;
  uint64_t meta;
} TzIntlNumber;

typedef struct {
  const TzType *type;
  const TzLayout *layout;
  int64_t size;
  int32_t offsets[TZ_IF_COUNT];
} TzIntlShape;

#define TZ_INTL_TEXT 48
#define TZ_INTL_LOCALE 96

typedef struct TzIntl {
  int64_t rc;
  const TzLayout *layout;
  int32_t kind;
  char locale[TZ_INTL_LOCALE];
  char text[TZ_IO_COUNT][TZ_INTL_TEXT];
  double number[TZ_IO_COUNT];
} TzIntl;

const TzIcu *tz_intl_icu(void);
TZ_COLD TZ_NORETURN void tz_intl_no_icu(void);

int tz_intl_ascii(TzStr s, char *out, size_t size);
void tz_intl_locale_tag(TzStr *out, void *tag_obj, uint64_t tag_meta, TzMaker make);
void tz_intl_locale_part(TzStr *out, void *tag_obj, uint64_t tag_meta, int32_t part,
                         TzMaker make);
void tz_intl_copy(char *out, size_t size, const char *text);
const TzUChar *tz_intl_utf16(TzStr *s, TzUChar *buffer, int32_t capacity, TzUChar **heap,
                             int32_t *length);

int tz_intl_resolve_locale(const TzIcu *icu, int has_locale, TzStr locale, char *requested,
                           char *found);
void tz_intl_keyword(const TzIcu *icu, const char *requested, const char *bcp_key, char *out,
                     size_t size);
void tz_intl_tag(const TzIcu *icu, const char *id, char *out, size_t size);
int tz_intl_is_keyword_type(const char *text);
int tz_intl_has_numbering(const TzIcu *icu, const char *name);

const char *tz_intl_option(const TzIntlOptions *o, int index, const char *const *allowed, char *out,
                           const char *method, int *failed);
int32_t tz_intl_number_option(const TzIntlOptions *o, int index, int32_t min, int32_t max,
                              int32_t fallback, int *failed);
int tz_intl_bool_option(const TzIntlOptions *o, int index);

TzIntl *tz_intl_alloc(size_t size, int32_t kind, const TzLayout *layout);
void tz_intl_set_text(TzIntl *o, int index, const char *text);

void tz_intl_push_part(TzArray *parts, const TzIntlShape *shape, const char *type,
                       const TzUChar *value, int32_t length, const char *unit);
void tz_intl_push_value_parts(TzArray *parts, const TzIntlShape *shape, const void *value,
                              int32_t category, const char *(*type_of)(int32_t field, void *ctx),
                              void *ctx, const char *unit);

TzIntl *tz_intl_cached(int32_t kind, const TzIntlOptions *o, const char *method);

TzIntl *tz_nf_create(const TzIntlOptions *o, const char *method);
TzIntl *tz_collator_create(const TzIntlOptions *o, const char *method);
TzIntl *tz_pr_create(const TzIntlOptions *o);
TzIntl *tz_rtf_create(const TzIntlOptions *o);
TzIntl *tz_lf_create(const TzIntlOptions *o);
TzIntl *tz_dn_create(const TzIntlOptions *o);
TzIntl *tz_seg_create(const TzIntlOptions *o);

int tz_nf_digit_options(const TzIntlOptions *o, TzIntl *r, int mnfd_default, int mxfd_default,
                        int compact, const char *method, char *skeleton, size_t size);
void tz_nf_format_into(const void *formatter, const TzIntlNumber *n, void *result,
                       TzUErrorCode *status);

void tz_intl_shutdown(void);

#endif

#endif
