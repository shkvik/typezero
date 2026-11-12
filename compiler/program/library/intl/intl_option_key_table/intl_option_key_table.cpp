#include "compiler/program/library/intl/intl_option_key_table/intl_option_key_table.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

const IntlOptionKeyInfo kRows[] = {
    {
        .name = "localeMatcher",
        .value = IntlOptionKey::LOCALE_MATCHER,
    },
    {
        .name = "numberingSystem",
        .value = IntlOptionKey::NUMBERING_SYSTEM,
    },
    {
        .name = "style",
        .value = IntlOptionKey::STYLE,
    },
    {
        .name = "currency",
        .value = IntlOptionKey::CURRENCY,
    },
    {
        .name = "currencyDisplay",
        .value = IntlOptionKey::CURRENCY_DISPLAY,
    },
    {
        .name = "currencySign",
        .value = IntlOptionKey::CURRENCY_SIGN,
    },
    {
        .name = "unit",
        .value = IntlOptionKey::UNIT,
    },
    {
        .name = "unitDisplay",
        .value = IntlOptionKey::UNIT_DISPLAY,
    },
    {
        .name = "useGrouping",
        .value = IntlOptionKey::USE_GROUPING,
    },
    {
        .name = "notation",
        .value = IntlOptionKey::NOTATION,
    },
    {
        .name = "compactDisplay",
        .value = IntlOptionKey::COMPACT_DISPLAY,
    },
    {
        .name = "signDisplay",
        .value = IntlOptionKey::SIGN_DISPLAY,
    },
    {
        .name = "minimumIntegerDigits",
        .value = IntlOptionKey::MINIMUM_INTEGER_DIGITS,
    },
    {
        .name = "minimumFractionDigits",
        .value = IntlOptionKey::MINIMUM_FRACTION_DIGITS,
    },
    {
        .name = "maximumFractionDigits",
        .value = IntlOptionKey::MAXIMUM_FRACTION_DIGITS,
    },
    {
        .name = "minimumSignificantDigits",
        .value = IntlOptionKey::MINIMUM_SIGNIFICANT_DIGITS,
    },
    {
        .name = "maximumSignificantDigits",
        .value = IntlOptionKey::MAXIMUM_SIGNIFICANT_DIGITS,
    },
    {
        .name = "roundingIncrement",
        .value = IntlOptionKey::ROUNDING_INCREMENT,
    },
    {
        .name = "roundingMode",
        .value = IntlOptionKey::ROUNDING_MODE,
    },
    {
        .name = "roundingPriority",
        .value = IntlOptionKey::ROUNDING_PRIORITY,
    },
    {
        .name = "trailingZeroDisplay",
        .value = IntlOptionKey::TRAILING_ZERO_DISPLAY,
    },
    {
        .name = "type",
        .value = IntlOptionKey::TYPE,
    },
    {
        .name = "usage",
        .value = IntlOptionKey::USAGE,
    },
    {
        .name = "sensitivity",
        .value = IntlOptionKey::SENSITIVITY,
    },
    {
        .name = "ignorePunctuation",
        .value = IntlOptionKey::IGNORE_PUNCTUATION,
    },
    {
        .name = "collation",
        .value = IntlOptionKey::COLLATION,
    },
    {
        .name = "numeric",
        .value = IntlOptionKey::NUMERIC,
    },
    {
        .name = "caseFirst",
        .value = IntlOptionKey::CASE_FIRST,
    },
    {
        .name = "fallback",
        .value = IntlOptionKey::FALLBACK,
    },
    {
        .name = "languageDisplay",
        .value = IntlOptionKey::LANGUAGE_DISPLAY,
    },
    {
        .name = "granularity",
        .value = IntlOptionKey::GRANULARITY,
    },
};

}

std::optional<IntlOptionKey> IntlOptionKeyTable::find(llvm::StringRef name) {
  for (const IntlOptionKeyInfo &info : kRows) {
    if (info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

llvm::StringRef IntlOptionKeyTable::getName(IntlOptionKey value) {
  for (const IntlOptionKeyInfo &info : kRows) {
    if (info.value == value) {
      return info.name;
    }
  }
  llvm_unreachable("intl option key missing from the table");
}

}
