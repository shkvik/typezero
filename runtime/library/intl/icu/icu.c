// Loading the ICU of the system (see icu.h).

#include "runtime/library/intl/icu/icu.h"
#include "runtime/core/tzrt/tzrt.h"

#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// None of this is static: see the rules in tzrt.h.
TzIcu tz_icu_functions;
int32_t tz_icu_state; // 0 not tried yet, 1 loaded, -1 none
char tz_icu_locale[96];
int32_t tz_icu_missing; // of the functions of Intl beyond dates

#ifdef _WIN32

static int load(void) {
  HMODULE common = LoadLibraryExW(L"icu.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
  HMODULE i18n = common;
  if (!common) {
    common = LoadLibraryExW(L"icuuc.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    i18n = LoadLibraryExW(L"icuin.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!common || !i18n)
      return 0;
  }
  TzIcu *f = &tz_icu_functions;
  int ok = 1;
#define REQUIRED(module, name) ok &= (*(FARPROC *)&f->name = GetProcAddress(module, #name)) != NULL
#define LOAD_OPTIONAL(module, name) *(FARPROC *)&f->name = GetProcAddress(module, #name)
#define LOAD_EITHER(name)                                                                          \
  if (!(*(FARPROC *)&f->name = GetProcAddress(common, #name)) &&                                   \
      !(*(FARPROC *)&f->name = GetProcAddress(i18n, #name)))                                       \
  ++tz_icu_missing
  REQUIRED(i18n, ucal_open);
  REQUIRED(i18n, ucal_close);
  REQUIRED(i18n, ucal_clone);
  REQUIRED(i18n, ucal_setMillis);
  REQUIRED(i18n, ucal_get);
  REQUIRED(i18n, ucal_getTimeZoneTransitionDate);
  LOAD_OPTIONAL(i18n, ucal_getTimeZoneOffsetFromLocal);
  REQUIRED(i18n, ucal_getTimeZoneDisplayName);
  REQUIRED(i18n, ucal_getTimeZoneID);
  REQUIRED(i18n, ucal_getCanonicalTimeZoneID);
  REQUIRED(i18n, ucal_setGregorianChange);
  REQUIRED(i18n, ucal_getTimeZoneIDForWindowsID);
  REQUIRED(i18n, ucal_getType);
  REQUIRED(i18n, ucal_openTimeZones);
  REQUIRED(common, uenum_unext);
  REQUIRED(common, uenum_close);
  REQUIRED(common, uloc_getDefault);
  REQUIRED(common, uloc_getBaseName);
  REQUIRED(common, uloc_forLanguageTag);
  REQUIRED(common, uloc_toLanguageTag);
  REQUIRED(common, uloc_getLanguage);
  REQUIRED(common, uloc_getKeywordValue);
  REQUIRED(common, uloc_setKeywordValue);
  REQUIRED(common, uloc_countAvailable);
  REQUIRED(common, uloc_getAvailable);
  REQUIRED(common, uloc_toUnicodeLocaleType);
  REQUIRED(common, uloc_canonicalize);
  REQUIRED(common, uloc_addLikelySubtags);
  REQUIRED(i18n, unumsys_open);
  REQUIRED(i18n, unumsys_getName);
  REQUIRED(i18n, unumsys_close);
  REQUIRED(i18n, udat_open);
  REQUIRED(i18n, udat_close);
  REQUIRED(i18n, udat_format);
  REQUIRED(i18n, udat_formatForFields);
  REQUIRED(i18n, udat_toPattern);
  REQUIRED(i18n, udat_getCalendar);
  REQUIRED(i18n, udat_setCalendar);
  REQUIRED(i18n, ufieldpositer_open);
  REQUIRED(i18n, ufieldpositer_close);
  REQUIRED(i18n, ufieldpositer_next);
  REQUIRED(i18n, udatpg_open);
  REQUIRED(i18n, udatpg_close);
  REQUIRED(i18n, udatpg_getBestPatternWithOptions);
  REQUIRED(i18n, udatpg_getSkeleton);
  LOAD_OPTIONAL(i18n, udatpg_getDefaultHourCycle);
  // The rest of Intl, in whichever of the two libraries has them: those
  // missing are counted, and tz_intl_icu checks.
  LOAD_EITHER(unumf_openForSkeletonAndLocaleWithError);
  LOAD_EITHER(unumf_close);
  LOAD_EITHER(unumf_openResult);
  LOAD_EITHER(unumf_closeResult);
  LOAD_EITHER(unumf_formatInt);
  LOAD_EITHER(unumf_formatDouble);
  LOAD_EITHER(unumf_formatDecimal);
  LOAD_EITHER(unumf_resultAsValue);
  LOAD_EITHER(ufmtval_getString);
  LOAD_EITHER(ufmtval_nextPosition);
  LOAD_EITHER(ucfpos_open);
  LOAD_EITHER(ucfpos_close);
  LOAD_EITHER(ucfpos_constrainCategory);
  LOAD_EITHER(ucfpos_getField);
  LOAD_EITHER(ucfpos_getIndexes);
  LOAD_EITHER(ucurr_getDefaultFractionDigits);
  LOAD_EITHER(ucurr_getName);
  LOAD_EITHER(unumsys_openByName);
  LOAD_EITHER(unumsys_isAlgorithmic);
  LOAD_EITHER(uplrules_openForType);
  LOAD_EITHER(uplrules_close);
  LOAD_EITHER(uplrules_selectFormatted);
  LOAD_EITHER(uplrules_getKeywords);
  LOAD_EITHER(uenum_next);
  LOAD_EITHER(ucol_open);
  LOAD_EITHER(ucol_close);
  LOAD_EITHER(ucol_strcoll);
  LOAD_EITHER(ucol_setAttribute);
  LOAD_EITHER(ucol_getAttribute);
  LOAD_EITHER(ucol_getKeywordValuesForLocale);
  LOAD_EITHER(unorm2_getNFCInstance);
  LOAD_EITHER(unorm2_getNFDInstance);
  LOAD_EITHER(unorm2_getNFKCInstance);
  LOAD_EITHER(unorm2_getNFKDInstance);
  LOAD_EITHER(unorm2_normalize);
  LOAD_EITHER(unorm2_spanQuickCheckYes);
  LOAD_EITHER(ubrk_open);
  LOAD_EITHER(ubrk_close);
  LOAD_EITHER(ubrk_first);
  LOAD_EITHER(ubrk_next);
  LOAD_EITHER(ubrk_following);
  LOAD_EITHER(ubrk_preceding);
  LOAD_EITHER(ubrk_isBoundary);
  LOAD_EITHER(ubrk_getRuleStatus);
  LOAD_EITHER(ureldatefmt_open);
  LOAD_EITHER(ureldatefmt_close);
  LOAD_EITHER(ureldatefmt_openResult);
  LOAD_EITHER(ureldatefmt_closeResult);
  LOAD_EITHER(ureldatefmt_resultAsValue);
  LOAD_EITHER(ureldatefmt_formatNumericToResult);
  LOAD_EITHER(ureldatefmt_formatToResult);
  LOAD_EITHER(ulistfmt_openForType);
  LOAD_EITHER(ulistfmt_close);
  LOAD_EITHER(ulistfmt_openResult);
  LOAD_EITHER(ulistfmt_closeResult);
  LOAD_EITHER(ulistfmt_resultAsValue);
  LOAD_EITHER(ulistfmt_formatStringsToResult);
  LOAD_EITHER(uldn_openForContext);
  LOAD_EITHER(uldn_close);
  LOAD_EITHER(uldn_localeDisplayName);
  LOAD_EITHER(uldn_regionDisplayName);
  LOAD_EITHER(uldn_scriptDisplayName);
  LOAD_EITHER(uldn_keyValueDisplayName);
  LOAD_EITHER(udatpg_getFieldDisplayName);
  LOAD_EITHER(uloc_toLegacyType);
#undef REQUIRED
#undef LOAD_OPTIONAL
#undef LOAD_EITHER
  return ok;
}

#else

static int load(void) { return 0; }

#endif

// The threads load ICU once, under the lock; the state goes last.
TzLock tz_icu_lock;

const TzIcu *tz_icu(void) {
  if (__atomic_load_n(&tz_icu_state, __ATOMIC_ACQUIRE) == 0) {
    tz_lock(&tz_icu_lock);
    if (tz_icu_state != 0) {
      tz_unlock(&tz_icu_lock);
      return tz_icu_state > 0 ? &tz_icu_functions : NULL;
    }
    int32_t state = load() ? 1 : -1;
    if (state > 0) {
      // Windows adds the user's settings as keywords
      // ("ru_RU@calendar=gregorian;hours=h23;..."); node's ICU knows only the
      // language, and so do we.
      TzUErrorCode status = 0;
      int32_t n =
          tz_icu_functions.uloc_getBaseName(tz_icu_functions.uloc_getDefault(), tz_icu_locale,
                                            (int32_t)sizeof tz_icu_locale - 1, &status);
      if (TZ_U_FAILURE(status) || n <= 0)
        strcpy(tz_icu_locale, "en_US");
    }
    __atomic_store_n(&tz_icu_state, state, __ATOMIC_RELEASE);
    tz_unlock(&tz_icu_lock);
  }
  return tz_icu_state > 0 ? &tz_icu_functions : NULL;
}

const char *tz_icu_default_locale(void) { return tz_icu() ? tz_icu_locale : "en_US"; }
