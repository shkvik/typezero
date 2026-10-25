// The ICU of the system, for time zones and languages (docs/date.md,
// sections 2 and 3; docs/intl.md). In Windows 10 1903 and later it is
// icu.dll; Windows 10 before that has the same functions in icuuc.dll and
// icuin.dll. The library is loaded when the program first needs it, so a
// program that uses only UTC and Date.now() never loads it.
//
// TODO(linux): Linux and macOS have no ICU to count on. There the zone
// database goes into the executable (about 150 KB, only for programs that use
// local time or zones), and the data of the languages by the rule of
// docs/date.md, section 3. Until then they have UTC and no languages.

#ifndef TZ_ICU_H
#define TZ_ICU_H

#include <stdint.h>

// The few types of ICU's C API that the runtime uses; the handles are opaque.
typedef uint16_t TzUChar;
typedef int TzUErrorCode; // a failure is > 0
#define TZ_U_FAILURE(status) ((status) > 0)
#define TZ_U_BUFFER_OVERFLOW_ERROR 15

// UCalendarDateFields and other constants of the C API.
enum {
  TZ_UCAL_GREGORIAN = 1,
  TZ_UCAL_ZONE_OFFSET = 15,
  TZ_UCAL_DST_OFFSET = 16,
  TZ_UCAL_STANDARD = 0, // display names
  TZ_UCAL_DST = 2,
  TZ_UCAL_TZ_TRANSITION_NEXT = 0,
  TZ_UCAL_TZ_TRANSITION_PREVIOUS_INCLUSIVE = 3,
  TZ_UCAL_TZ_LOCAL_FORMER = 4,
  TZ_UDAT_PATTERN = -2,
  TZ_UDAT_NONE = -1,
  TZ_UDATPG_MATCH_HOUR_FIELD_LENGTH = 1 << 11,
};

typedef struct TzIcu {
  // ucal
  void *(*ucal_open)(const TzUChar *zone, int32_t length, const char *locale, int type,
                     TzUErrorCode *status);
  void (*ucal_close)(void *cal);
  void *(*ucal_clone)(const void *cal, TzUErrorCode *status);
  void (*ucal_setMillis)(void *cal, double ms, TzUErrorCode *status);
  int32_t (*ucal_get)(const void *cal, int field, TzUErrorCode *status);
  int8_t (*ucal_getTimeZoneTransitionDate)(const void *cal, int type, double *transition,
                                           TzUErrorCode *status);
  // ICU 69 and later; NULL before.
  void (*ucal_getTimeZoneOffsetFromLocal)(const void *cal, int nonexistent, int duplicated,
                                          int32_t *raw, int32_t *dst, TzUErrorCode *status);
  int32_t (*ucal_getTimeZoneDisplayName)(const void *cal, int type, const char *locale,
                                         TzUChar *result, int32_t capacity, TzUErrorCode *status);
  int32_t (*ucal_getTimeZoneID)(const void *cal, TzUChar *result, int32_t capacity,
                                TzUErrorCode *status);
  int32_t (*ucal_getCanonicalTimeZoneID)(const TzUChar *id, int32_t length, TzUChar *result,
                                         int32_t capacity, int8_t *is_system, TzUErrorCode *status);
  void (*ucal_setGregorianChange)(void *cal, double date, TzUErrorCode *status);
  int32_t (*ucal_getTimeZoneIDForWindowsID)(const TzUChar *windows, int32_t length,
                                            const char *region, TzUChar *id, int32_t capacity,
                                            TzUErrorCode *status);
  const char *(*ucal_getType)(const void *cal, TzUErrorCode *status);
  void *(*ucal_openTimeZones)(TzUErrorCode *status);
  const TzUChar *(*uenum_unext)(void *enumeration, int32_t *length, TzUErrorCode *status);
  void (*uenum_close)(void *enumeration);
  // uloc
  const char *(*uloc_getDefault)(void);
  int32_t (*uloc_getBaseName)(const char *locale, char *name, int32_t capacity,
                              TzUErrorCode *status);
  int32_t (*uloc_forLanguageTag)(const char *tag, char *locale, int32_t capacity, int32_t *parsed,
                                 TzUErrorCode *status);
  int32_t (*uloc_toLanguageTag)(const char *locale, char *tag, int32_t capacity, int8_t strict,
                                TzUErrorCode *status);
  int32_t (*uloc_getLanguage)(const char *locale, char *language, int32_t capacity,
                              TzUErrorCode *status);
  int32_t (*uloc_getKeywordValue)(const char *locale, const char *keyword, char *value,
                                  int32_t capacity, TzUErrorCode *status);
  int32_t (*uloc_setKeywordValue)(const char *keyword, const char *value, char *locale,
                                  int32_t capacity, TzUErrorCode *status);
  int32_t (*uloc_countAvailable)(void);
  const char *(*uloc_getAvailable)(int32_t index);
  const char *(*uloc_toUnicodeLocaleType)(const char *keyword, const char *value);
  int32_t (*uloc_canonicalize)(const char *locale, char *name, int32_t capacity,
                               TzUErrorCode *status);
  int32_t (*uloc_addLikelySubtags)(const char *locale, char *maximized, int32_t capacity,
                                   TzUErrorCode *status);
  void *(*unumsys_open)(const char *locale, TzUErrorCode *status);
  const char *(*unumsys_getName)(const void *system);
  void (*unumsys_close)(void *system);
  // udat
  void *(*udat_open)(int time_style, int date_style, const char *locale, const TzUChar *zone,
                     int32_t zone_length, const TzUChar *pattern, int32_t pattern_length,
                     TzUErrorCode *status);
  void (*udat_close)(void *format);
  int32_t (*udat_format)(const void *format, double date, TzUChar *result, int32_t capacity,
                         void *position, TzUErrorCode *status);
  int32_t (*udat_formatForFields)(const void *format, double date, TzUChar *result,
                                  int32_t capacity, void *iterator, TzUErrorCode *status);
  int32_t (*udat_toPattern)(const void *format, int8_t localized, TzUChar *result, int32_t capacity,
                            TzUErrorCode *status);
  const void *(*udat_getCalendar)(const void *format);
  void (*udat_setCalendar)(void *format, const void *cal);
  void *(*ufieldpositer_open)(TzUErrorCode *status);
  void (*ufieldpositer_close)(void *iterator);
  int32_t (*ufieldpositer_next)(void *iterator, int32_t *begin, int32_t *end);
  // udatpg
  void *(*udatpg_open)(const char *locale, TzUErrorCode *status);
  void (*udatpg_close)(void *generator);
  int32_t (*udatpg_getBestPatternWithOptions)(void *generator, const TzUChar *skeleton,
                                              int32_t length, int options, TzUChar *pattern,
                                              int32_t capacity, TzUErrorCode *status);
  int32_t (*udatpg_getSkeleton)(void *generator, const TzUChar *pattern, int32_t length,
                                TzUChar *skeleton, int32_t capacity, TzUErrorCode *status);
  // ICU 67 and later; NULL before.
  int (*udatpg_getDefaultHourCycle)(const void *generator, TzUErrorCode *status);

  // The rest of Intl (intl.c and the files after it): NULL where the system
  // lacks them, which tz_intl_icu checks.
  // unumf, the number formatter of skeletons (ICU 67 and later)
  void *(*unumf_openForSkeletonAndLocaleWithError)(const TzUChar *skeleton, int32_t length,
                                                   const char *locale, void *parse_error,
                                                   TzUErrorCode *status);
  void (*unumf_close)(void *formatter);
  void *(*unumf_openResult)(TzUErrorCode *status);
  void (*unumf_closeResult)(void *result);
  void (*unumf_formatInt)(const void *formatter, int64_t value, void *result, TzUErrorCode *status);
  void (*unumf_formatDouble)(const void *formatter, double value, void *result,
                             TzUErrorCode *status);
  void (*unumf_formatDecimal)(const void *formatter, const char *value, int32_t length,
                              void *result, TzUErrorCode *status);
  const void *(*unumf_resultAsValue)(const void *result, TzUErrorCode *status);
  // ufmtval and ucfpos: the text of a formatted value and its fields
  const TzUChar *(*ufmtval_getString)(const void *value, int32_t *length, TzUErrorCode *status);
  int8_t (*ufmtval_nextPosition)(const void *value, void *position, TzUErrorCode *status);
  void *(*ucfpos_open)(TzUErrorCode *status);
  void (*ucfpos_close)(void *position);
  void (*ucfpos_constrainCategory)(void *position, int32_t category, TzUErrorCode *status);
  int32_t (*ucfpos_getField)(const void *position, TzUErrorCode *status);
  void (*ucfpos_getIndexes)(const void *position, int32_t *start, int32_t *limit,
                            TzUErrorCode *status);
  // ucurr, unumsys
  int32_t (*ucurr_getDefaultFractionDigits)(const TzUChar *currency, TzUErrorCode *status);
  const TzUChar *(*ucurr_getName)(const TzUChar *currency, const char *locale, int style,
                                  int8_t *is_choice, int32_t *length, TzUErrorCode *status);
  void *(*unumsys_openByName)(const char *name, TzUErrorCode *status);
  int8_t (*unumsys_isAlgorithmic)(const void *system);
  // uplrules
  void *(*uplrules_openForType)(const char *locale, int type, TzUErrorCode *status);
  void (*uplrules_close)(void *rules);
  int32_t (*uplrules_selectFormatted)(const void *rules, const void *number, TzUChar *keyword,
                                      int32_t capacity, TzUErrorCode *status);
  void *(*uplrules_getKeywords)(const void *rules, TzUErrorCode *status);
  const char *(*uenum_next)(void *enumeration, int32_t *length, TzUErrorCode *status);
  // ucol
  void *(*ucol_open)(const char *locale, TzUErrorCode *status);
  void (*ucol_close)(void *collator);
  int32_t (*ucol_strcoll)(const void *collator, const TzUChar *a, int32_t a_length,
                          const TzUChar *b, int32_t b_length);
  void (*ucol_setAttribute)(void *collator, int attribute, int value, TzUErrorCode *status);
  int (*ucol_getAttribute)(const void *collator, int attribute, TzUErrorCode *status);
  void *(*ucol_getKeywordValuesForLocale)(const char *key, const char *locale, int8_t common,
                                          TzUErrorCode *status);
  // unorm2
  const void *(*unorm2_getNFCInstance)(TzUErrorCode *status);
  const void *(*unorm2_getNFDInstance)(TzUErrorCode *status);
  const void *(*unorm2_getNFKCInstance)(TzUErrorCode *status);
  const void *(*unorm2_getNFKDInstance)(TzUErrorCode *status);
  int32_t (*unorm2_normalize)(const void *normalizer, const TzUChar *text, int32_t length,
                              TzUChar *out, int32_t capacity, TzUErrorCode *status);
  int32_t (*unorm2_spanQuickCheckYes)(const void *normalizer, const TzUChar *text, int32_t length,
                                      TzUErrorCode *status);
  // ubrk
  void *(*ubrk_open)(int type, const char *locale, const TzUChar *text, int32_t length,
                     TzUErrorCode *status);
  void (*ubrk_close)(void *iterator);
  int32_t (*ubrk_first)(void *iterator);
  int32_t (*ubrk_next)(void *iterator);
  int32_t (*ubrk_following)(void *iterator, int32_t offset);
  int32_t (*ubrk_preceding)(void *iterator, int32_t offset);
  int8_t (*ubrk_isBoundary)(void *iterator, int32_t offset);
  int32_t (*ubrk_getRuleStatus)(void *iterator);
  // ureldatefmt
  void *(*ureldatefmt_open)(const char *locale, void *number_format, int width, int context,
                            TzUErrorCode *status);
  void (*ureldatefmt_close)(void *formatter);
  void *(*ureldatefmt_openResult)(TzUErrorCode *status);
  void (*ureldatefmt_closeResult)(void *result);
  const void *(*ureldatefmt_resultAsValue)(const void *result, TzUErrorCode *status);
  void (*ureldatefmt_formatNumericToResult)(const void *formatter, double offset, int unit,
                                            void *result, TzUErrorCode *status);
  void (*ureldatefmt_formatToResult)(const void *formatter, double offset, int unit, void *result,
                                     TzUErrorCode *status);
  // ulistfmt
  void *(*ulistfmt_openForType)(const char *locale, int type, int width, TzUErrorCode *status);
  void (*ulistfmt_close)(void *formatter);
  void *(*ulistfmt_openResult)(TzUErrorCode *status);
  void (*ulistfmt_closeResult)(void *result);
  const void *(*ulistfmt_resultAsValue)(const void *result, TzUErrorCode *status);
  void (*ulistfmt_formatStringsToResult)(const void *formatter, const TzUChar *const *strings,
                                         const int32_t *lengths, int32_t count, void *result,
                                         TzUErrorCode *status);
  // uldn, the names of languages, regions, scripts and keywords
  void *(*uldn_openForContext)(const char *locale, int *contexts, int32_t count,
                               TzUErrorCode *status);
  void (*uldn_close)(void *names);
  int32_t (*uldn_localeDisplayName)(const void *names, const char *locale, TzUChar *result,
                                    int32_t capacity, TzUErrorCode *status);
  int32_t (*uldn_regionDisplayName)(const void *names, const char *region, TzUChar *result,
                                    int32_t capacity, TzUErrorCode *status);
  int32_t (*uldn_scriptDisplayName)(const void *names, const char *script, TzUChar *result,
                                    int32_t capacity, TzUErrorCode *status);
  int32_t (*uldn_keyValueDisplayName)(const void *names, const char *key, const char *value,
                                      TzUChar *result, int32_t capacity, TzUErrorCode *status);
  int32_t (*udatpg_getFieldDisplayName)(const void *generator, int field, int width, TzUChar *name,
                                        int32_t capacity, TzUErrorCode *status);
  const char *(*uloc_toLegacyType)(const char *keyword, const char *value);
} TzIcu;

// The functions of the ICU of the system, loaded on the first call; NULL if
// there is none.
const TzIcu *tz_icu(void);
// How many of the functions of Intl beyond dates the system lacks (an ICU
// older than 67).
extern int32_t tz_icu_missing;

// The language of the system as ICU names it, without its keywords
// ("ru_RU"), as node has it.
const char *tz_icu_default_locale(void);

// The IANA name of the local zone (date_zone.c), as
// Intl.DateTimeFormat().resolvedOptions() gives it; its length.
int32_t tz_date_zone_id(TzUChar *out, int32_t capacity);

#endif
