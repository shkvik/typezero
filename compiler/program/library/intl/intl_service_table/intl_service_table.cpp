#include "compiler/program/library/intl/intl_service_table/intl_service_table.h"
#include "runtime/library/intl/intl/intl.h"
#include "llvm/ADT/Twine.h"

namespace tz {

namespace {

constexpr llvm::StringLiteral kIntlPrefix = "Intl.";
constexpr llvm::StringLiteral kResolvedPrefix = "Resolved";
constexpr llvm::StringLiteral kOptionsSuffix = "Options";

const std::vector<IntlService> kServices = {
    {
        .kind = IntlKind::NumberFormat,
        .name = "NumberFormat",
        .runtimeKind = TZ_INTL_NUMBER_FORMAT,
        .options =
            {
                {IntlOptionKey::LOCALE_MATCHER, IntlOptionKind::TEXT, {"lookup", "best fit"}},
                {IntlOptionKey::NUMBERING_SYSTEM, IntlOptionKind::TEXT, {}},
                {IntlOptionKey::STYLE,
                 IntlOptionKind::TEXT,
                 {"decimal", "percent", "currency", "unit"}},
                {IntlOptionKey::CURRENCY, IntlOptionKind::TEXT, {}},
                {IntlOptionKey::CURRENCY_DISPLAY,
                 IntlOptionKind::TEXT,
                 {"code", "symbol", "narrowSymbol", "name"}},
                {IntlOptionKey::CURRENCY_SIGN, IntlOptionKind::TEXT, {"standard", "accounting"}},
                {IntlOptionKey::UNIT, IntlOptionKind::TEXT, {}},
                {IntlOptionKey::UNIT_DISPLAY, IntlOptionKind::TEXT, {"short", "narrow", "long"}},
                {IntlOptionKey::USE_GROUPING,
                 IntlOptionKind::GROUPING,
                 {"min2", "auto", "always", "true", "false"}},
                {IntlOptionKey::NOTATION,
                 IntlOptionKind::TEXT,
                 {"standard", "scientific", "engineering", "compact"}},
                {IntlOptionKey::COMPACT_DISPLAY, IntlOptionKind::TEXT, {"short", "long"}},
                {IntlOptionKey::SIGN_DISPLAY,
                 IntlOptionKind::TEXT,
                 {"auto", "never", "always", "exceptZero", "negative"}},
                {IntlOptionKey::MINIMUM_INTEGER_DIGITS, IntlOptionKind::NUMBER, {}},
                {IntlOptionKey::MINIMUM_FRACTION_DIGITS, IntlOptionKind::NUMBER, {}},
                {IntlOptionKey::MAXIMUM_FRACTION_DIGITS, IntlOptionKind::NUMBER, {}},
                {IntlOptionKey::MINIMUM_SIGNIFICANT_DIGITS, IntlOptionKind::NUMBER, {}},
                {IntlOptionKey::MAXIMUM_SIGNIFICANT_DIGITS, IntlOptionKind::NUMBER, {}},
                {IntlOptionKey::ROUNDING_INCREMENT, IntlOptionKind::NUMBER, {}},
                {IntlOptionKey::ROUNDING_MODE,
                 IntlOptionKind::TEXT,
                 {"ceil", "floor", "expand", "trunc", "halfCeil", "halfFloor", "halfExpand",
                  "halfTrunc", "halfEven"}},
                {IntlOptionKey::ROUNDING_PRIORITY,
                 IntlOptionKind::TEXT,
                 {"auto", "morePrecision", "lessPrecision"}},
                {IntlOptionKey::TRAILING_ZERO_DISPLAY,
                 IntlOptionKind::TEXT,
                 {"auto", "stripIfInteger"}},
            },
        .callable = true,
    },
    {
        .kind = IntlKind::Collator,
        .name = "Collator",
        .runtimeKind = TZ_INTL_COLLATOR,
        .options =
            {
                {IntlOptionKey::LOCALE_MATCHER, IntlOptionKind::TEXT, {"lookup", "best fit"}},
                {IntlOptionKey::USAGE, IntlOptionKind::TEXT, {"sort", "search"}},
                {IntlOptionKey::SENSITIVITY,
                 IntlOptionKind::TEXT,
                 {"base", "accent", "case", "variant"}},
                {IntlOptionKey::IGNORE_PUNCTUATION, IntlOptionKind::FLAG, {}},
                {IntlOptionKey::COLLATION, IntlOptionKind::TEXT, {}},
                {IntlOptionKey::NUMERIC, IntlOptionKind::FLAG, {}},
                {IntlOptionKey::CASE_FIRST, IntlOptionKind::TEXT, {"upper", "lower", "false"}},
            },
        .callable = true,
    },
    {
        .kind = IntlKind::PluralRules,
        .name = "PluralRules",
        .runtimeKind = TZ_INTL_PLURAL_RULES,
        .options =
            {
                {IntlOptionKey::LOCALE_MATCHER, IntlOptionKind::TEXT, {"lookup", "best fit"}},
                {IntlOptionKey::TYPE, IntlOptionKind::TEXT, {"cardinal", "ordinal"}},
                {IntlOptionKey::MINIMUM_INTEGER_DIGITS, IntlOptionKind::NUMBER, {}},
                {IntlOptionKey::MINIMUM_FRACTION_DIGITS, IntlOptionKind::NUMBER, {}},
                {IntlOptionKey::MAXIMUM_FRACTION_DIGITS, IntlOptionKind::NUMBER, {}},
                {IntlOptionKey::MINIMUM_SIGNIFICANT_DIGITS, IntlOptionKind::NUMBER, {}},
                {IntlOptionKey::MAXIMUM_SIGNIFICANT_DIGITS, IntlOptionKind::NUMBER, {}},
                {IntlOptionKey::ROUNDING_INCREMENT, IntlOptionKind::NUMBER, {}},
                {IntlOptionKey::ROUNDING_MODE,
                 IntlOptionKind::TEXT,
                 {"ceil", "floor", "expand", "trunc", "halfCeil", "halfFloor", "halfExpand",
                  "halfTrunc", "halfEven"}},
                {IntlOptionKey::ROUNDING_PRIORITY,
                 IntlOptionKind::TEXT,
                 {"auto", "morePrecision", "lessPrecision"}},
                {IntlOptionKey::TRAILING_ZERO_DISPLAY,
                 IntlOptionKind::TEXT,
                 {"auto", "stripIfInteger"}},
            },
        .callable = false,
    },
    {
        .kind = IntlKind::RelativeTimeFormat,
        .name = "RelativeTimeFormat",
        .runtimeKind = TZ_INTL_RELATIVE_TIME_FORMAT,
        .options =
            {
                {IntlOptionKey::LOCALE_MATCHER, IntlOptionKind::TEXT, {"lookup", "best fit"}},
                {IntlOptionKey::NUMBERING_SYSTEM, IntlOptionKind::TEXT, {}},
                {IntlOptionKey::STYLE, IntlOptionKind::TEXT, {"long", "short", "narrow"}},
                {IntlOptionKey::NUMERIC, IntlOptionKind::TEXT, {"always", "auto"}},
            },
        .callable = false,
    },
    {
        .kind = IntlKind::ListFormat,
        .name = "ListFormat",
        .runtimeKind = TZ_INTL_LIST_FORMAT,
        .options =
            {
                {IntlOptionKey::LOCALE_MATCHER, IntlOptionKind::TEXT, {"lookup", "best fit"}},
                {IntlOptionKey::TYPE, IntlOptionKind::TEXT, {"conjunction", "disjunction", "unit"}},
                {IntlOptionKey::STYLE, IntlOptionKind::TEXT, {"long", "short", "narrow"}},
            },
        .callable = false,
    },
    {
        .kind = IntlKind::DisplayNames,
        .name = "DisplayNames",
        .runtimeKind = TZ_INTL_DISPLAY_NAMES,
        .options =
            {
                {IntlOptionKey::LOCALE_MATCHER, IntlOptionKind::TEXT, {"lookup", "best fit"}},
                {IntlOptionKey::STYLE, IntlOptionKind::TEXT, {"long", "short", "narrow"}},
                {IntlOptionKey::TYPE,
                 IntlOptionKind::TEXT,
                 {"language", "region", "script", "currency", "calendar", "dateTimeField"}},
                {IntlOptionKey::FALLBACK, IntlOptionKind::TEXT, {"code", "none"}},
                {IntlOptionKey::LANGUAGE_DISPLAY, IntlOptionKind::TEXT, {"dialect", "standard"}},
            },
        .callable = false,
    },
    {
        .kind = IntlKind::Segmenter,
        .name = "Segmenter",
        .runtimeKind = TZ_INTL_SEGMENTER,
        .options =
            {
                {IntlOptionKey::LOCALE_MATCHER, IntlOptionKind::TEXT, {"lookup", "best fit"}},
                {IntlOptionKey::GRANULARITY,
                 IntlOptionKind::TEXT,
                 {"grapheme", "word", "sentence"}},
            },
        .callable = false,
    },
};

const IntlService kSupportedLocales = {
    .kind = IntlKind::None,
    .name = "supportedLocalesOf",
    .runtimeKind = 0,
    .options = {{IntlOptionKey::LOCALE_MATCHER, IntlOptionKind::TEXT, {"lookup", "best fit"}}},
    .callable = true,
};

const IntlDigitRange kDigitRanges[] = {
    {.key = IntlOptionKey::MINIMUM_INTEGER_DIGITS, .low = 1, .high = 21},
    {.key = IntlOptionKey::MINIMUM_FRACTION_DIGITS, .low = 0, .high = 100},
    {.key = IntlOptionKey::MAXIMUM_FRACTION_DIGITS, .low = 0, .high = 100},
    {.key = IntlOptionKey::MINIMUM_SIGNIFICANT_DIGITS, .low = 1, .high = 21},
    {.key = IntlOptionKey::MAXIMUM_SIGNIFICANT_DIGITS, .low = 1, .high = 21},
};

constexpr int64_t kRoundingIncrements[] = {1,   2,   5,   10,   20,   25,   50,  100,
                                           200, 250, 500, 1000, 2000, 2500, 5000};

}

const std::vector<IntlService> &IntlServiceTable::getServices() {
  return kServices;
}

const IntlService *IntlServiceTable::find(llvm::StringRef name) {
  for (const IntlService &service : kServices) {
    if (service.name == name) {
      return &service;
    }
  }

  return nullptr;
}

const IntlService *IntlServiceTable::findKind(IntlKind kind) {
  for (const IntlService &service : kServices) {
    if (service.kind == kind) {
      return &service;
    }
  }

  return nullptr;
}

const IntlService &IntlServiceTable::getSupportedLocalesService() {
  return kSupportedLocales;
}

const IntlService *IntlServiceTable::findByOptionsTypeName(llvm::StringRef name) {
  for (const IntlService &service : kServices) {
    if (name == (service.name + kOptionsSuffix).str()) {
      return &service;
    }
  }

  return nullptr;
}

const IntlService *IntlServiceTable::findByResolvedTypeName(llvm::StringRef name) {
  for (const IntlService &service : kServices) {
    if (name == (kResolvedPrefix + service.name + kOptionsSuffix).str()) {
      return &service;
    }
  }

  return nullptr;
}

std::string IntlServiceTable::getOptionsTypeName(llvm::StringRef serviceName) {
  return (kIntlPrefix + serviceName + kOptionsSuffix).str();
}

std::string IntlServiceTable::getQualifiedName(llvm::StringRef name) {
  return (kIntlPrefix + name).str();
}

std::optional<IntlDigitRange> IntlServiceTable::findDigitRange(IntlOptionKey key) {
  for (const IntlDigitRange &range : kDigitRanges) {
    if (range.key == key) {
      return range;
    }
  }

  return std::nullopt;
}

llvm::ArrayRef<int64_t> IntlServiceTable::getRoundingIncrements() {
  return kRoundingIncrements;
}

}
