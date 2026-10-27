#include "runtime/library/date/date/date.h"
#include "runtime/library/intl/intl/intl.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/array/array.h"
#include "runtime/core/closure/closure.h"
#include "runtime/core/error/error.h"
#include "runtime/core/output/output.h"
#include "runtime/core/parallel/parallel.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  TzStr locale;
  TzStr values[TZ_DTO_COUNT];
  uint32_t present;
  int32_t hour12;
  double fractional;
} TzDateOptions;

enum { HC_NONE, HC_H11, HC_H12, HC_H23, HC_H24 };

#define TEXT 64
#define LOCALE_BIT (1u << 31)

typedef struct {
  void *format;
  char locale[TEXT];
  char values[TZ_DTO_COUNT][TEXT];
  int32_t hour12;
  int32_t fractional;
} Resolved;

typedef struct {
  int64_t rc;
  const TzLayout *layout;
  Resolved *resolved;
} TzDateFormat;

void tz_dtf_drop(void *object);
static const TzLayout dtf_layout = {tz_dtf_drop, NULL, 0, TZ_LAYOUT_DATE_FORMAT, NULL, 0, 0};

static const char *const allowed[TZ_DTO_COUNT][7] = {
    [TZ_DTO_LOCALE_MATCHER] = {"lookup", "best fit"},
    [TZ_DTO_HOUR_CYCLE] = {"h11", "h12", "h23", "h24"},
    [TZ_DTO_WEEKDAY] = {"narrow", "short", "long"},
    [TZ_DTO_ERA] = {"narrow", "short", "long"},
    [TZ_DTO_YEAR] = {"2-digit", "numeric"},
    [TZ_DTO_MONTH] = {"2-digit", "numeric", "narrow", "short", "long"},
    [TZ_DTO_DAY] = {"2-digit", "numeric"},
    [TZ_DTO_DAY_PERIOD] = {"narrow", "short", "long"},
    [TZ_DTO_HOUR] = {"2-digit", "numeric"},
    [TZ_DTO_MINUTE] = {"2-digit", "numeric"},
    [TZ_DTO_SECOND] = {"2-digit", "numeric"},
    [TZ_DTO_TIME_ZONE_NAME] = {"short", "long", "shortOffset", "longOffset", "shortGeneric",
                               "longGeneric"},
    [TZ_DTO_FORMAT_MATCHER] = {"basic", "best fit"},
    [TZ_DTO_DATE_STYLE] = {"full", "long", "medium", "short"},
    [TZ_DTO_TIME_STYLE] = {"full", "long", "medium", "short"},
};

static const char *const option_names[TZ_DTO_COUNT] = {
    "localeMatcher", "calendar", "numberingSystem", "hourCycle",     "timeZone",  "weekday",
    "era",           "year",     "month",           "day",           "dayPeriod", "hour",
    "minute",        "second",   "timeZoneName",    "formatMatcher", "dateStyle", "timeStyle",
};

TZ_COLD TZ_NORETURN static void no_icu(void) {
  tz_panicf("Intl needs the ICU of the system, which Windows has from Windows 10 1903 on");
}

static const char *method_name(int kind) {
  switch (kind) {
  case TZ_DATE_LOCALE_DATE:
    return "Date.prototype.toLocaleDateString";
  case TZ_DATE_LOCALE_TIME:
    return "Date.prototype.toLocaleTimeString";
  case TZ_DATE_LOCALE_FORMAT:
    return "Intl.DateTimeFormat";
  default:
    return "Date.prototype.toLocaleString";
  }
}

static void copy_text(char *out, const char *text) {
  size_t n = strlen(text);
  if (n >= TEXT) {
    n = TEXT - 1;
  }
  memcpy(out, text, n);
  out[n] = 0;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int equal_ignoring_case(const char *a, const char *b) {
  for (; *a && *b; ++a, ++b) {
    if (lower((unsigned char)*a) != lower((unsigned char)*b)) {
      return 0;
    }
  }

  return *a == *b;
}

static int32_t widen(const char *text, TzUChar *out, int32_t capacity) {
  int32_t n = 0;
  for (; text[n] && n < capacity - 1; ++n) {
    out[n] = (unsigned char)text[n];
  }
  out[n] = 0;

  return n;
}

static void narrow_text(const TzUChar *units, int32_t length, char *out, size_t size) {
  size_t n = 0;
  for (int32_t i = 0; i < length && n + 1 < size; ++i) {
    out[n++] = units[i] < 0x80 ? (char)units[i] : '?';
  }
  out[n] = 0;
}

static const char *option(const TzDateOptions *o, int index, char *out, int kind) {
  if (!(o->present & (1u << index))) {
    return NULL;
  }
  int ok = tz_intl_ascii(o->values[index], out, TEXT);
  if (!ok) {
    out[0] = 0;
  }
  if (allowed[index][0]) {
    int found = 0;
    for (int i = 0; ok && allowed[index][i] && !found; ++i) {
      found = strcmp(allowed[index][i], out) == 0;
    }
    if (!found) {
      tz_panicf("internal error: the value %s of the option %s of %s", ok ? out : "?",
                option_names[index], method_name(kind));
    }
  }

  return out;
}

static int resolve_zone(const TzIcu *icu, const char *name, char *resolved, TzUChar *id,
                        int32_t *id_length) {
  static const char *const utc[] = {"UTC", "GMT", "Etc/UTC", "Etc/GMT"};
  for (int i = 0; i < 4; ++i) {
    if (equal_ignoring_case(name, utc[i])) {
      copy_text(resolved, "UTC");
      *id_length = widen("UTC", id, TEXT);
      return 1;
    }
  }
  if (name[0] == '+' || name[0] == '-') {
    const char *p = name + 1;
    int digits[4], n = 0;
    for (; *p && n < 4; ++p) {
      if (*p == ':' && n == 2 && p[1]) {
        continue;
      }
      if (*p < '0' || *p > '9') {
        return 0;
      }
      digits[n++] = *p - '0';
    }
    if (*p || (n != 2 && n != 4)) {
      return 0;
    }
    int hours = digits[0] * 10 + digits[1], minutes = n == 4 ? digits[2] * 10 + digits[3] : 0;
    if (hours > 23 || minutes > 59 || (n == 2 && strchr(name, ':'))) {
      return 0;
    }
    char text[TEXT];
    text[0] = name[0];
    text[1] = (char)('0' + hours / 10);
    text[2] = (char)('0' + hours % 10);
    text[3] = ':';
    text[4] = (char)('0' + minutes / 10);
    text[5] = (char)('0' + minutes % 10);
    text[6] = 0;
    copy_text(resolved, text);
    char gmt[TEXT] = "GMT";
    strcat(gmt, text);
    *id_length = widen(gmt, id, TEXT);
    return 1;
  }
  TzUErrorCode status = 0;
  void *zones = icu->ucal_openTimeZones(&status);
  if (TZ_U_FAILURE(status)) {
    return 0;
  }
  int found = 0;
  int32_t length;
  const TzUChar *zone;
  while (!found && (zone = icu->uenum_unext(zones, &length, &status)) != NULL) {
    char text[TEXT];
    if (length >= TEXT) {
      continue;
    }
    narrow_text(zone, length, text, sizeof text);
    if (equal_ignoring_case(text, name)) {
      found = 1;
      int8_t system = 0;
      TzUErrorCode canonical = 0;
      TzUChar buffer[TEXT];
      int32_t n = icu->ucal_getCanonicalTimeZoneID(zone, length, buffer, TEXT, &system, &canonical);
      if (TZ_U_FAILURE(canonical) || n <= 0 || n >= TEXT) {
        memcpy(buffer, zone, (size_t)length * sizeof(TzUChar));
        n = length;
      }
      narrow_text(buffer, n, resolved, TEXT);
      if (strcmp(resolved, "Etc/UTC") == 0 || strcmp(resolved, "Etc/GMT") == 0) {
        copy_text(resolved, "UTC");
      }
      memcpy(id, zone, (size_t)length * sizeof(TzUChar));
      *id_length = length;
    }
  }
  icu->uenum_close(zones);

  return found;
}

static int hour_cycle_of(const TzUChar *pattern, int32_t length) {
  int quoted = 0;
  for (int32_t i = 0; i < length; ++i) {
    TzUChar c = pattern[i];
    if (c == '\'') {
      quoted = !quoted;
    } else if (!quoted && c == 'h') {
      return HC_H12;
    } else if (!quoted && c == 'H') {
      return HC_H23;
    } else if (!quoted && c == 'k') {
      return HC_H24;
    } else if (!quoted && c == 'K') {
      return HC_H11;
    }
  }

  return HC_NONE;
}

static TzUChar hour_char(int hc) {
  switch (hc) {
  case HC_H11:
    return 'K';
  case HC_H12:
    return 'h';
  case HC_H23:
    return 'H';
  case HC_H24:
    return 'k';
  default:
    return 'j';
  }
}

static void replace_hour_cycle(TzUChar *pattern, int32_t length, int hc) {
  if (hc == HC_NONE) {
    return;
  }
  int quoted = 0;
  for (int32_t i = 0; i < length; ++i) {
    TzUChar c = pattern[i];
    if (c == '\'') {
      quoted = !quoted;
    } else if (!quoted && (c == 'h' || c == 'H' || c == 'k' || c == 'K')) {
      pattern[i] = hour_char(hc);
    }
  }
}

static int32_t best_pattern(const TzIcu *icu, void *generator, const TzUChar *skeleton,
                            int32_t length, int hc, TzUChar *pattern, int32_t capacity) {
  TzUErrorCode status = 0;
  int32_t n = icu->udatpg_getBestPatternWithOptions(
      generator, skeleton, length, TZ_UDATPG_MATCH_HOUR_FIELD_LENGTH, pattern, capacity, &status);
  if (TZ_U_FAILURE(status) || n >= capacity) {
    return -1;
  }
  pattern[n] = 0;
  replace_hour_cycle(pattern, n, hc);

  return n;
}

static const char *const style_names[] = {"full", "long", "medium", "short"};

static int style_of(const char *value) {
  if (!value) {
    return TZ_UDAT_NONE;
  }
  for (int i = 0; i < 4; ++i) {
    if (strcmp(style_names[i], value) == 0) {
      return i;
    }
  }

  return TZ_UDAT_NONE;
}

static void resolve_parts(Resolved *r, const TzUChar *pattern, int32_t length) {
  char text[256];
  narrow_text(pattern, length, text, sizeof text);
  static const struct {
    int option;
    const char *letters[10];
    const char *values[10];
  } parts[] = {
      {TZ_DTO_WEEKDAY,
       {"EEEEE", "EEEE", "EEE", "ccccc", "cccc", "ccc"},
       {"narrow", "long", "short", "narrow", "long", "short"}},
      {TZ_DTO_ERA, {"GGGGG", "GGGG", "GGG"}, {"narrow", "long", "short"}},
      {TZ_DTO_YEAR, {"yy", "y"}, {"2-digit", "numeric"}},
      {TZ_DTO_MONTH,
       {"MMMMM", "MMMM", "MMM", "MM", "M", "LLLLL", "LLLL", "LLL", "LL", "L"},
       {"narrow", "long", "short", "2-digit", "numeric", "narrow", "long", "short", "2-digit",
        "numeric"}},
      {TZ_DTO_DAY, {"dd", "d"}, {"2-digit", "numeric"}},
      {TZ_DTO_DAY_PERIOD,
       {"BBBBB", "bbbbb", "BBBB", "bbbb", "B", "b"},
       {"narrow", "narrow", "long", "long", "short", "short"}},
      {TZ_DTO_HOUR,
       {"HH", "H", "hh", "h", "kk", "k", "KK", "K"},
       {"2-digit", "numeric", "2-digit", "numeric", "2-digit", "numeric", "2-digit", "numeric"}},
      {TZ_DTO_MINUTE, {"mm", "m"}, {"2-digit", "numeric"}},
      {TZ_DTO_SECOND, {"ss", "s"}, {"2-digit", "numeric"}},
      {TZ_DTO_TIME_ZONE_NAME,
       {"zzzz", "z", "OOOO", "O", "vvvv", "v"},
       {"long", "short", "longOffset", "shortOffset", "longGeneric", "shortGeneric"}},
  };
  for (size_t i = 0; i < sizeof parts / sizeof parts[0]; ++i) {
    for (int k = 0; k < 10 && parts[i].letters[k]; ++k) {
      if (strstr(text, parts[i].letters[k])) {
        copy_text(r->values[parts[i].option], parts[i].values[k]);
        break;
      }
    }
  }
  int digits = 0;
  for (const char *p = text; *p && digits < 3; ++p) {
    digits += *p == 'S';
  }
  r->fractional = digits;
}

static Resolved *create(const TzDateOptions *o, int kind) {
  const TzIcu *icu = tz_icu();
  if (!icu) {
    no_icu();
  }
  TzUErrorCode status = 0;

  char requested[160], found[TEXT];
  if (!tz_intl_resolve_locale(icu, (o->present & LOCALE_BIT) != 0, o->locale, requested, found)) {
    return NULL;
  }
  char keyword_calendar[TEXT] = "", keyword_numbers[TEXT] = "", keyword_hours[TEXT] = "";
  struct {
    const char *name;
    char *value;
  } keywords[] = {
      {"calendar", keyword_calendar}, {"numbers", keyword_numbers}, {"hours", keyword_hours}};
  for (int i = 0; i < 3; ++i) {
    status = 0;
    int32_t k = icu->uloc_getKeywordValue(requested, keywords[i].name, keywords[i].value, TEXT - 1,
                                          &status);
    keywords[i].value[TZ_U_FAILURE(status) || k < 0 ? 0 : k] = 0;
  }

  Resolved *r = (Resolved *)tz_alloc(sizeof(Resolved));
  memset(r, 0, sizeof *r);
  r->hour12 = -1;
  char buffers[TZ_DTO_COUNT][TEXT];
  const char *values[TZ_DTO_COUNT];
  for (int i = 0; i < TZ_DTO_COUNT; ++i) {
    values[i] = option(o, i, buffers[i], kind);
  }
  if (values[TZ_DTO_CALENDAR] && !tz_intl_is_keyword_type(values[TZ_DTO_CALENDAR])) {
    values[TZ_DTO_CALENDAR] = NULL;
  }
  if (values[TZ_DTO_NUMBERING_SYSTEM] &&
      !tz_intl_is_keyword_type(values[TZ_DTO_NUMBERING_SYSTEM])) {
    values[TZ_DTO_NUMBERING_SYSTEM] = NULL;
  }
  int fractional = 0;
  if (o->fractional >= 1 && o->fractional <= 3) {
    fractional = (int)floor(o->fractional);
  }

  char tag[TEXT * 2];
  status = 0;
  int32_t n = icu->uloc_toLanguageTag(found, tag, TEXT, 0, &status);
  tag[TZ_U_FAILURE(status) || n < 0 ? 0 : n] = 0;
  copy_text(r->locale, tag);
  char extension[TEXT * 2] = "", resolved_extension[TEXT * 2] = "";
  const char *calendar = values[TZ_DTO_CALENDAR];
  const char *numbers = values[TZ_DTO_NUMBERING_SYSTEM];
  if (!calendar && keyword_calendar[0]) {
    calendar = icu->uloc_toUnicodeLocaleType("ca", keyword_calendar);
    if (calendar) {
      strcat(resolved_extension, "-ca-");
      strcat(resolved_extension, calendar);
    }
  }
  int hc_keyword = HC_NONE;
  if (keyword_hours[0] && values[TZ_DTO_HOUR_CYCLE] == NULL && o->hour12 < 0) {
    hc_keyword = strcmp(keyword_hours, "h11") == 0   ? HC_H11
                 : strcmp(keyword_hours, "h12") == 0 ? HC_H12
                 : strcmp(keyword_hours, "h23") == 0 ? HC_H23
                 : strcmp(keyword_hours, "h24") == 0 ? HC_H24
                                                     : HC_NONE;
    if (hc_keyword != HC_NONE) {
      strcat(resolved_extension, "-hc-");
      strcat(resolved_extension, keyword_hours);
    }
  }
  if (!numbers && keyword_numbers[0]) {
    numbers = icu->uloc_toUnicodeLocaleType("nu", keyword_numbers);
    if (numbers) {
      strcat(resolved_extension, "-nu-");
      strcat(resolved_extension, numbers);
    }
  }
  if (calendar && strlen(calendar) < TEXT) {
    strcat(extension, "-ca-");
    strcat(extension, calendar);
  }
  if (keyword_hours[0] && strlen(keyword_hours) < 8) {
    strcat(extension, "-hc-");
    strcat(extension, keyword_hours);
  }
  if (numbers && strlen(numbers) < TEXT) {
    strcat(extension, "-nu-");
    strcat(extension, numbers);
  }
  if (resolved_extension[0] && strlen(r->locale) + strlen(resolved_extension) + 3 < TEXT) {
    strcat(r->locale, "-u");
    strcat(r->locale, resolved_extension);
  }
  char locale[160];
  copy_text(locale, found);
  if (extension[0]) {
    char full[TEXT * 3];
    strcpy(full, tag);
    strcat(full, "-u");
    strcat(full, extension);
    status = 0;
    int32_t parsed;
    n = icu->uloc_forLanguageTag(full, locale, (int32_t)sizeof locale - 1, &parsed, &status);
    if (TZ_U_FAILURE(status) || n <= 0) {
      copy_text(locale, found);
    } else {
      locale[n] = 0;
    }
  }

  TzUChar zone[TEXT];
  int32_t zone_length = 0;
  if (!values[TZ_DTO_TIME_ZONE] || !resolve_zone(icu, values[TZ_DTO_TIME_ZONE],
                                                 r->values[TZ_DTO_TIME_ZONE], zone, &zone_length)) {
    zone_length = tz_date_zone_id(zone, TEXT);
    narrow_text(zone, zone_length, r->values[TZ_DTO_TIME_ZONE], TEXT);
  }

  status = 0;
  void *generator = icu->udatpg_open(locale, &status);
  if (TZ_U_FAILURE(status)) {
    no_icu();
  }
  int hc_default = HC_NONE;
  if (icu->udatpg_getDefaultHourCycle) {
    status = 0;
    int cycle = icu->udatpg_getDefaultHourCycle(generator, &status);
    if (!TZ_U_FAILURE(status)) {
      hc_default = cycle == 0 ? HC_H11 : cycle == 1 ? HC_H12 : cycle == 2 ? HC_H23 : HC_H24;
    }
  }
  if (hc_default == HC_NONE) {
    TzUChar j[2] = {'j', 0}, pattern[TEXT];
    int32_t length = best_pattern(icu, generator, j, 1, HC_NONE, pattern, TEXT);
    hc_default = length > 0 ? hour_cycle_of(pattern, length) : HC_H23;
    if (hc_default == HC_NONE) {
      hc_default = HC_H23;
    }
  }
  const char *cycle = values[TZ_DTO_HOUR_CYCLE];
  int hc_option = !cycle || o->hour12 >= 0    ? HC_NONE
                  : strcmp(cycle, "h11") == 0 ? HC_H11
                  : strcmp(cycle, "h12") == 0 ? HC_H12
                  : strcmp(cycle, "h23") == 0 ? HC_H23
                                              : HC_H24;
  int hc12 = hc_default == HC_H11 || hc_default == HC_H12 ? hc_default : HC_H12;
  int hc24 = hc_default == HC_H23 || hc_default == HC_H24 ? hc_default : HC_H23;
  int hc = hc_option != HC_NONE    ? hc_option
           : o->hour12 > 0         ? hc12
           : o->hour12 == 0        ? hc24
           : hc_keyword != HC_NONE ? hc_keyword
                                   : hc_default;
  int twelve = o->hour12 >= 0 ? o->hour12 > 0 : hc_option != HC_NONE ? hc_option <= HC_H12 : -1;
  int hc_defaults = hc_keyword;
  if (twelve > 0) {
    char language[16] = "";
    status = 0;
    icu->uloc_getLanguage(locale, language, (int32_t)sizeof language - 1, &status);
    hc_defaults = hc12 == HC_H12 && strcmp(language, "ja") == 0 ? HC_H11 : hc12;
  } else if (twelve == 0) {
    hc_defaults = hc24;
  }

  const char *date_style = values[TZ_DTO_DATE_STYLE], *time_style = values[TZ_DTO_TIME_STYLE];
  if (kind == TZ_DATE_LOCALE_DATE) {
    time_style = values[TZ_DTO_TIME_STYLE] = NULL;
  } else if (kind == TZ_DATE_LOCALE_TIME) {
    date_style = values[TZ_DTO_DATE_STYLE] = NULL;
  }
  if (date_style || time_style) {
    fractional = 0;
    for (int i = TZ_DTO_WEEKDAY; i <= TZ_DTO_TIME_ZONE_NAME; ++i) {
      values[i] = NULL;
    }
  }
  TzUChar pattern[256];
  int32_t pattern_length = -1;
  void *format = NULL;
  if (date_style || time_style) {
    status = 0;
    format = icu->udat_open(style_of(time_style), style_of(date_style), locale, zone, zone_length,
                            NULL, -1, &status);
    if (TZ_U_FAILURE(status)) {
      no_icu();
    }
    status = 0;
    pattern_length = icu->udat_toPattern(format, 0, pattern, 255, &status);
    if (time_style && !TZ_U_FAILURE(status) && pattern_length < 255 &&
        hc != hour_cycle_of(pattern, pattern_length)) {
      TzUChar skeleton[256], replaced[256];
      status = 0;
      int32_t length =
          icu->udatpg_getSkeleton(NULL, pattern, pattern_length, skeleton, 255, &status);
      int32_t m = 0;
      for (int32_t i = 0; !TZ_U_FAILURE(status) && i < length && length < 255; ++i) {
        TzUChar c = skeleton[i];
        if (c == 'a' || c == 'b' || c == 'B') {
          continue;
        }
        replaced[m++] = c == 'h' || c == 'H' || c == 'k' || c == 'K' ? hour_char(hc) : c;
      }
      int32_t made = best_pattern(icu, generator, replaced, m, hc, pattern, 255);
      if (made > 0) {
        icu->udat_close(format);
        status = 0;
        format = icu->udat_open(TZ_UDAT_PATTERN, TZ_UDAT_PATTERN, locale, zone, zone_length,
                                pattern, made, &status);
        if (TZ_U_FAILURE(status)) {
          no_icu();
        }
        pattern_length = made;
      }
    }
    if (date_style) {
      copy_text(r->values[TZ_DTO_DATE_STYLE], date_style);
    }
    if (time_style) {
      copy_text(r->values[TZ_DTO_TIME_STYLE], time_style);
    }
  } else {
    int need_defaults = 1;
    if (kind != TZ_DATE_LOCALE_TIME) {
      for (int i = TZ_DTO_WEEKDAY; i <= TZ_DTO_DAY; ++i) {
        if (i != TZ_DTO_ERA && values[i]) {
          need_defaults = 0;
        }
      }
    }
    if (kind != TZ_DATE_LOCALE_DATE) {
      for (int i = TZ_DTO_DAY_PERIOD; i <= TZ_DTO_SECOND; ++i) {
        if (values[i]) {
          need_defaults = 0;
        }
      }
      if (fractional) {
        need_defaults = 0;
      }
    }
    if (need_defaults && kind != TZ_DATE_LOCALE_TIME) {
      values[TZ_DTO_YEAR] = values[TZ_DTO_MONTH] = values[TZ_DTO_DAY] = "numeric";
    }
    int hour_defaulted =
        need_defaults && (kind == TZ_DATE_LOCALE_ALL || kind == TZ_DATE_LOCALE_TIME);
    if (hour_defaulted) {
      values[TZ_DTO_HOUR] = values[TZ_DTO_MINUTE] = values[TZ_DTO_SECOND] = "numeric";
    }
    int hc_skeleton = hour_defaulted ? hc_defaults : hc;
    static const struct {
      int option;
      const char *values[6];
      const char *letters[6];
    } parts[] = {
        {TZ_DTO_WEEKDAY, {"narrow", "long", "short"}, {"EEEEE", "EEEE", "EEE"}},
        {TZ_DTO_ERA, {"narrow", "long", "short"}, {"GGGGG", "GGGG", "GGG"}},
        {TZ_DTO_YEAR, {"2-digit", "numeric"}, {"yy", "y"}},
        {TZ_DTO_MONTH,
         {"narrow", "long", "short", "2-digit", "numeric"},
         {"MMMMM", "MMMM", "MMM", "MM", "M"}},
        {TZ_DTO_DAY, {"2-digit", "numeric"}, {"dd", "d"}},
        {TZ_DTO_DAY_PERIOD, {"narrow", "long", "short"}, {"BBBBB", "BBBB", "B"}},
        {TZ_DTO_HOUR, {"2-digit", "numeric"}, {"", ""}},
        {TZ_DTO_MINUTE, {"2-digit", "numeric"}, {"mm", "m"}},
        {TZ_DTO_SECOND, {"2-digit", "numeric"}, {"ss", "s"}},
        {TZ_DTO_TIME_ZONE_NAME,
         {"long", "short", "longOffset", "shortOffset", "longGeneric", "shortGeneric"},
         {"zzzz", "z", "OOOO", "O", "vvvv", "v"}},
    };
    TzUChar skeleton[64];
    int32_t length = 0;
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; ++i) {
      int index = parts[i].option;
      if (index == TZ_DTO_TIME_ZONE_NAME) {
        for (int k = 0; k < fractional; ++k) {
          skeleton[length++] = 'S';
        }
      }
      const char *value = values[index];
      if (!value) {
        continue;
      }
      for (int k = 0; k < 6 && parts[i].values[k]; ++k) {
        if (strcmp(parts[i].values[k], value) != 0) {
          continue;
        }
        if (index == TZ_DTO_HOUR) {
          skeleton[length++] = hour_char(hc_skeleton);
          if (k == 0) {
            skeleton[length++] = hour_char(hc_skeleton);
          }
        } else {
          for (const char *p = parts[i].letters[k]; *p; ++p) {
            skeleton[length++] = (TzUChar)*p;
          }
        }
        break;
      }
    }
    skeleton[length] = 0;
    int hc_pattern = values[TZ_DTO_HOUR] && !hour_defaulted ? hc : HC_NONE;
    pattern_length = best_pattern(icu, generator, skeleton, length, hc_pattern, pattern, 255);
    if (pattern_length < 0) {
      no_icu();
    }
    status = 0;
    format = icu->udat_open(TZ_UDAT_PATTERN, TZ_UDAT_PATTERN, locale, zone, zone_length, pattern,
                            pattern_length, &status);
    if (TZ_U_FAILURE(status)) {
      no_icu();
    }
    resolve_parts(r, pattern, pattern_length);
  }
  icu->udatpg_close(generator);

  status = 0;
  const void *cal = icu->udat_getCalendar(format);
  const char *type = icu->ucal_getType(cal, &status);
  if (!TZ_U_FAILURE(status) && type && strcmp(type, "gregorian") == 0) {
    void *copy = icu->ucal_clone(cal, &status);
    if (!TZ_U_FAILURE(status)) {
      icu->ucal_setGregorianChange(copy, -9007199254740992.0, &status);
      icu->udat_setCalendar(format, copy);
      icu->ucal_close(copy);
    }
  }
  r->format = format;

  status = 0;
  type = icu->ucal_getType(icu->udat_getCalendar(format), &status);
  const char *bcp =
      type && !TZ_U_FAILURE(status) ? icu->uloc_toUnicodeLocaleType("ca", type) : NULL;
  copy_text(r->values[TZ_DTO_CALENDAR], bcp ? bcp : "gregory");
  status = 0;
  void *system = icu->unumsys_open(locale, &status);
  copy_text(r->values[TZ_DTO_NUMBERING_SYSTEM],
            !TZ_U_FAILURE(status) && system ? icu->unumsys_getName(system) : "latn");
  if (system) {
    icu->unumsys_close(system);
  }
  int hc_shown = pattern_length > 0 ? hour_cycle_of(pattern, pattern_length) : HC_NONE;
  if (hc_shown != HC_NONE) {
    static const char *const cycles[] = {"", "h11", "h12", "h23", "h24"};
    copy_text(r->values[TZ_DTO_HOUR_CYCLE], cycles[hc_shown]);
    r->hour12 = hc_shown == HC_H11 || hc_shown == HC_H12;
  }

  return r;
}

static void release(Resolved *r) {
  tz_icu()->udat_close(r->format);
  tz_free(r);
}

static int32_t format_units(Resolved *r, double t, TzUChar *buffer, int32_t capacity,
                            TzUChar **heap, void *fields) {
  const TzIcu *icu = tz_icu();
  TzUErrorCode status = 0;
  *heap = NULL;
  int32_t n = fields ? icu->udat_formatForFields(r->format, t, buffer, capacity, fields, &status)
                     : icu->udat_format(r->format, t, buffer, capacity, NULL, &status);
  if (status == TZ_U_BUFFER_OVERFLOW_ERROR || n >= capacity) {
    *heap = (TzUChar *)tz_alloc(((size_t)n + 1) * sizeof(TzUChar));
    status = 0;
    n = fields ? icu->udat_formatForFields(r->format, t, *heap, n + 1, fields, &status)
               : icu->udat_format(r->format, t, *heap, n + 1, NULL, &status);
  }
  if (TZ_U_FAILURE(status)) {
    n = 0;
  }

  return n;
}

static TzStr format_string(Resolved *r, double t) {
  TzUChar buffer[256], *heap;
  int32_t n = format_units(r, t, buffer, 256, &heap, NULL);
  TzUChar *text = heap ? heap : buffer;
  for (int32_t i = 0; i < n; ++i) {
    if (text[i] == 0x202F) {
      text[i] = ' ';
    }
  }
  TzStr s = tz_str_from_utf16(heap ? heap : buffer, (size_t)n);
  tz_free(heap);

  return s;
}

#define CACHED 4
typedef struct {
  TzBuf key;
  Resolved *resolved;
} CacheEntry;
typedef struct {
  CacheEntry entries[CACHED];
  uint32_t next;
} Cache;

static Cache *cache_of(TzThread *t) {
  Cache *c = (Cache *)t->caches[TZ_CACHE_DATE_FORMAT];
  if (!c) {
    c = (Cache *)calloc(1, sizeof *c);
    if (!c) {
      tz_panic("out of memory", 13);
    }
    t->caches[TZ_CACHE_DATE_FORMAT] = c;
  }

  return c;
}

static void key_of(TzBuf *key, int kind, const TzDateOptions *o) {
  tz_buf_push(key, (uint16_t)kind);
  tz_buf_push(key, (uint16_t)(o->present >> 16));
  tz_buf_push(key, (uint16_t)o->present);
  tz_buf_push(key, (uint16_t)(o->hour12 + 1));
  tz_buf_push(key, (uint16_t)(isnan(o->fractional) ? 0xFFFF : (uint16_t)o->fractional));
  for (int i = -1; i < TZ_DTO_COUNT; ++i) {
    uint32_t bit = i < 0 ? LOCALE_BIT : 1u << i;
    if (!(o->present & bit)) {
      continue;
    }
    TzStr s = i < 0 ? o->locale : o->values[i];
    uint32_t n;
    int two;
    const void *units = tz_str_units(&s, &n, &two);
    tz_buf_push(key, (uint16_t)(n >> 16));
    tz_buf_push(key, (uint16_t)n);
    if (two) {
      tz_buf_units(key, (const uint16_t *)units, n);
    } else {
      tz_buf_bytes(key, (const uint8_t *)units, n);
    }
  }
}

static Resolved *cached(int kind, const TzDateOptions *o) {
  TzBuf key;
  tz_buf_init(&key);
  key_of(&key, kind, o);
  Cache *cache = cache_of(&tz_thread);
  for (int i = 0; i < CACHED; ++i) {
    CacheEntry *e = &cache->entries[i];
    if (e->resolved && e->key.length == key.length &&
        memcmp(e->key.data, key.data, key.length * sizeof(uint16_t)) == 0) {
      tz_buf_free(&key);
      return e->resolved;
    }
  }
  Resolved *r = create(o, kind);
  if (!r) {
    tz_buf_free(&key);
    return NULL;
  }
  CacheEntry *e = &cache->entries[cache->next++ % CACHED];
  if (e->resolved) {
    release(e->resolved);
    tz_buf_free(&e->key);
  }
  e->resolved = r;
  e->key = key;

  return r;
}

void tz_date_format_shutdown(void) {
  for (int32_t t = 0; t < tz_thread_total(); ++t) {
    TzThread *thread = tz_thread_at(t);
    Cache *cache = thread ? (Cache *)thread->caches[TZ_CACHE_DATE_FORMAT] : NULL;
    if (!cache) {
      continue;
    }
    for (int i = 0; i < CACHED; ++i) {
      CacheEntry *e = &cache->entries[i];
      if (e->resolved) {
        release(e->resolved);
        tz_buf_free(&e->key);
        e->resolved = NULL;
      }
    }
  }
}

void tz_date_locale(TzStr *out, double t, int32_t kind, const TzDateOptions *o) {
  Resolved *r = cached(kind, o);
  *out = r ? format_string(r, t) : tz_str_from_ascii("", 0);
}

void *tz_dtf_new(const TzDateOptions *o) {
  Resolved *r = create(o, TZ_DATE_LOCALE_FORMAT);
  if (!r) {
    return NULL;
  }
  TzDateFormat *f = (TzDateFormat *)tz_obj_alloc(sizeof(TzDateFormat));
  f->rc = 1;
  f->layout = &dtf_layout;
  f->resolved = r;

  return f;
}

void tz_dtf_drop(void *object) {
  TzDateFormat *f = (TzDateFormat *)object;
  release(f->resolved);
  tz_obj_free(f);
}

void tz_dtf_format(TzStr *out, TzDateFormat *f, double t) { *out = format_string(f->resolved, t); }

static const char *part_type(int32_t field) {
  switch (field) {
  case 0:
    return "era";
  case 1:
  case 20:
    return "year";
  case 30:
    return "yearName";
  case 2:
  case 26:
    return "month";
  case 3:
    return "day";
  case 4:
  case 5:
  case 15:
  case 16:
    return "hour";
  case 6:
    return "minute";
  case 7:
    return "second";
  case 8:
    return "fractionalSecond";
  case 9:
  case 19:
  case 25:
    return "weekday";
  case 14:
  case 35:
  case 36:
    return "dayPeriod";
  case 17:
  case 23:
  case 24:
  case 29:
  case 31:
  case 32:
  case 33:
    return "timeZoneName";
  case 34:
    return "relatedYear";
  default:
    return "literal";
  }
}

TzArray *tz_dtf_format_to_parts(TzDateFormat *f, double t, const TzType *part_type_desc,
                                const TzLayout *layout, int64_t size, uint32_t type_offset,
                                uint32_t value_offset) {
  const TzIcu *icu = tz_icu();
  TzUErrorCode status = 0;
  void *iterator = icu->ufieldpositer_open(&status);
  TzUChar buffer[256], *heap;
  int32_t n = format_units(f->resolved, t, buffer, 256, &heap, iterator);
  const TzUChar *text = heap ? heap : buffer;
  TzArray *parts = tz_arr_new(part_type_desc, 0, 8);
  int32_t previous = 0, begin, end, field;
  for (;;) {
    field = icu->ufieldpositer_next(iterator, &begin, &end);
    int last = field < 0;
    if (last) {
      begin = end = n;
    }
    for (int k = 0; k < 2; ++k) {
      int32_t from = k == 0 ? previous : begin, to = k == 0 ? begin : end;
      if (from >= to) {
        continue;
      }
      TzInstance *part = (TzInstance *)tz_obj_alloc(size);
      memset(part, 0, (size_t)size);
      part->rc = 1;
      part->layout = layout;
      const char *type = k == 0 ? "literal" : part_type(field);
      *(TzStr *)((char *)part + type_offset) = tz_str_from_ascii(type, strlen(type));
      *(TzStr *)((char *)part + value_offset) = tz_str_from_utf16(text + from, (size_t)(to - from));
      tz_arr_push_value(parts, &part);
    }
    if (last) {
      break;
    }
    previous = end;
  }
  icu->ufieldpositer_close(iterator);
  tz_free(heap);

  return parts;
}

int32_t tz_dtf_option(TzStr *out, TzDateFormat *f, int32_t which) {
  const char *text = which < 0 ? f->resolved->locale : f->resolved->values[which];
  *out = tz_str_from_ascii(text, strlen(text));

  return text[0] != 0;
}

int32_t tz_dtf_hour12(TzDateFormat *f) { return f->resolved->hour12; }

int32_t tz_dtf_fractional(TzDateFormat *f) { return f->resolved->fractional; }
