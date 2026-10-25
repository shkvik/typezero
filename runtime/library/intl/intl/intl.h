// Intl beyond dates (docs/intl.md): what the compiled code and the runtime
// share, and what the files of the runtime share among themselves.
//
// The objects of Intl (Intl.NumberFormat, Intl.Collator and the others) are
// objects of the runtime: the compiled code only holds them, calls their
// methods and reads what resolvedOptions() gives. Each starts as TzIntl does
// (in tzrt.h order: the count, then a layout of its own, whose drop function
// closes what ICU made for it).
//
// The data of the languages is the ICU of the system (icu.h) in Windows.
// TODO(linux): the data of the languages named in the code (or all of them,
// for a language known only when the program runs) embedded in the
// executable, by the rule of docs/date.md, section 3, and only the parts the
// program uses: numbers, collation, plural rules...

#ifndef TZ_INTL_H
#define TZ_INTL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The kinds of objects, as the compiled code names them to tz_intl_new.
enum {
  TZ_INTL_NUMBER_FORMAT = 1,
  TZ_INTL_COLLATOR = 2,
  TZ_INTL_PLURAL_RULES = 3,
  TZ_INTL_RELATIVE_TIME_FORMAT = 4,
  TZ_INTL_LIST_FORMAT = 5,
  TZ_INTL_DISPLAY_NAMES = 6,
  TZ_INTL_SEGMENTER = 7,
  TZ_INTL_SEGMENTS = 8, // what segment(text) gives
};

// The options of all of them, by the place each has in TzIntlOptions. Sema
// checks which a kind takes and of what type (sema_intl.cpp); a value is a
// string, a number, or a boolean as the number 0 or 1, and useGrouping is a
// string or a boolean.
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

// The fields of the objects the runtime makes for the program
// (TzIntlShape): the parts of formatToParts, the data of a segment.
enum {
  TZ_IF_TYPE,
  TZ_IF_VALUE,
  TZ_IF_UNIT, // of a part of RelativeTimeFormat: string | undefined
  TZ_IF_SEGMENT,
  TZ_IF_INDEX, // int
  TZ_IF_INPUT,
  TZ_IF_WORD_LIKE, // boolean | undefined
  TZ_IF_COUNT
};

// What the number of toLocaleString, format and select is (TzIntlNumber).
enum { TZ_IN_DOUBLE = 0, TZ_IN_INTEGER = 1, TZ_IN_DECIMAL = 2 };

#ifdef __cplusplus
}
#endif

// What follows is for the runtime only.
#ifndef __cplusplus

#include "icu.h"
#include "tzrt.h"

// The locale and the options as the compiled code passes them
// (CodeGen::emitIntlOptions); the strings are borrowed.
typedef struct {
  TzStr locale;
  TzStr text[TZ_IO_COUNT];
  double number[TZ_IO_COUNT];
  uint64_t has_text, has_number; // bit TZ_IO_* for a value given
  int32_t has_locale;
} TzIntlOptions;

// A number of the program: a double, an int or a long, or a decimal (its
// two words in `integer` and `meta`).
typedef struct {
  int32_t kind; // TZ_IN_*
  double d;
  int64_t integer;
  uint64_t meta;
} TzIntlNumber;

// How the program lays out an object that the runtime makes for it: what
// an array of them holds, its layout and size, and the offset of each of
// its fields (TZ_IF_*), -1 for those it does not have. A constant the
// compiler emits (CodeGen::intlShape).
typedef struct {
  const TzType *type;
  const TzLayout *layout;
  int64_t size;
  int32_t offsets[TZ_IF_COUNT];
} TzIntlShape;

#define TZ_INTL_TEXT 48
#define TZ_INTL_LOCALE 96

// The start of every object of Intl: the options as resolvedOptions() gives
// them, "" or NaN for those not there (a boolean is 0 or 1; useGrouping
// false is the text "false").
typedef struct TzIntl {
  int64_t rc;
  const TzLayout *layout; // with TZ_LAYOUT_INTL and the name of the kind
  int32_t kind;
  char locale[TZ_INTL_LOCALE];
  char text[TZ_IO_COUNT][TZ_INTL_TEXT];
  double number[TZ_IO_COUNT];
} TzIntl;

// The functions of the system's ICU, or a failure of the runtime when there
// is none or it is too old.
const TzIcu *tz_intl_icu(void);
TZ_COLD TZ_NORETURN void tz_intl_no_icu(void);

// The ASCII text of a string; 0 if it has other characters or does not fit.
int tz_intl_ascii(TzStr s, char *out, size_t size);
void tz_intl_copy(char *out, size_t size, const char *text);
// UTF-16 of a string, in `buffer` if it fits there, else in a block to
// free with tz_free (`*heap`); its length in `*length`.
const TzUChar *tz_intl_utf16(TzStr *s, TzUChar *buffer, int32_t capacity, TzUChar **heap,
                             int32_t *length);

// The language a locale asks for, as ICU names it with its keywords
// ("de_DE@collation=phonebook"), in `requested` (160 bytes), and the one ICU
// has data for that is closest to it (64 bytes: itself, or what is left
// without its last parts, else the language of the system), in `found`.
// Without a locale, the language of the system. 0 for a locale that is no
// language tag, with a RangeError thrown.
int tz_intl_resolve_locale(const TzIcu *icu, int has_locale, TzStr locale, char *requested,
                           char *found);
// The value of a keyword of `requested` in BCP 47 ("nu" → "arab"); "" if
// it has none.
void tz_intl_keyword(const TzIcu *icu, const char *requested, const char *bcp_key, char *out,
                     size_t size);
// The BCP 47 tag of an ICU locale ID ("de_DE" → "de-DE").
void tz_intl_tag(const TzIcu *icu, const char *id, char *out, size_t size);
// A type of a keyword as BCP 47 writes them: 3 to 8 letters and digits,
// parts joined by "-" ("gregory", "islamic-civil", "latn").
int tz_intl_is_keyword_type(const char *text);
// Whether the system has the numbering system (and it is not algorithmic).
int tz_intl_has_numbering(const TzIcu *icu, const char *name);

// A string option, checked against `allowed` (NULL-terminated; NULL for any
// value): NULL when not given; with a value that is not one, NULL with a
// RangeError thrown and `*failed` set. `method` names the service in the
// message.
const char *tz_intl_option(const TzIntlOptions *o, int index, const char *const *allowed, char *out,
                           const char *method, int *failed);
// A number option (GetNumberOption): `fallback` when not given; floor of a
// value from min to max, else a RangeError and `*failed`.
int32_t tz_intl_number_option(const TzIntlOptions *o, int index, int32_t min, int32_t max,
                              int32_t fallback, int *failed);
// A boolean option: -1 when not given.
int tz_intl_bool_option(const TzIntlOptions *o, int index);

// A new object of `kind` (its size, at least sizeof(TzIntl)), its options
// cleared, with the layout given.
TzIntl *tz_intl_alloc(size_t size, int32_t kind, const TzLayout *layout);
void tz_intl_set_text(TzIntl *o, int index, const char *text);

// Makes the part of formatToParts, `{ type, value }` (and `unit` if the
// shape has it and `unit` is given), pushed onto `parts`.
void tz_intl_push_part(TzArray *parts, const TzIntlShape *shape, const char *type,
                       const TzUChar *value, int32_t length, const char *unit);
// The parts of what ICU wrote: each field of `category` found in `value`
// (a UFormattedValue) is a part, the innermost where they nest, and the text
// between them literals; `type_of` names the type of a field.
void tz_intl_push_value_parts(TzArray *parts, const TzIntlShape *shape, const void *value,
                              int32_t category, const char *(*type_of)(int32_t field, void *ctx),
                              void *ctx, const char *unit);

// The NumberFormat or the Collator (`kind`) of toLocaleString or
// localeCompare for the options, kept for the next calls; NULL with an error
// thrown. `method` names the method in the messages.
TzIntl *tz_intl_cached(int32_t kind, const TzIntlOptions *o, const char *method);

// The kinds' own parts of tz_intl_new.
TzIntl *tz_nf_create(const TzIntlOptions *o, const char *method);
TzIntl *tz_collator_create(const TzIntlOptions *o, const char *method);
TzIntl *tz_pr_create(const TzIntlOptions *o);
TzIntl *tz_rtf_create(const TzIntlOptions *o);
TzIntl *tz_lf_create(const TzIntlOptions *o);
TzIntl *tz_dn_create(const TzIntlOptions *o);
TzIntl *tz_seg_create(const TzIntlOptions *o);

// The digit options of NumberFormat and PluralRules (ECMA-402,
// SetNumberFormatDigitOptions) as the skeleton of ICU's number formatter
// gives them ("precision-integer"...), appended to `skeleton`; the options
// resolved go into `r`. 0 with an error thrown.
int tz_nf_digit_options(const TzIntlOptions *o, TzIntl *r, int mnfd_default, int mxfd_default,
                        int compact, const char *method, char *skeleton, size_t size);
// Formats a number with a UNumberFormatter into `result`, a
// UFormattedNumber.
void tz_nf_format_into(const void *formatter, const TzIntlNumber *n, void *result,
                       TzUErrorCode *status);

#endif

#endif
