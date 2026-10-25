#include "compiler/library/intl/intl_check_helpers/intl_check_helpers.h"
#include "runtime/library/intl/intl/intl.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

std::vector<IntlOption> numberOptions() {
  std::vector<IntlOption> options = {
      kMatcher,
      kNumbering,
      {"style", 's', {"decimal", "percent", "currency", "unit"}},
      {"currency", 's', {}},
      {"currencyDisplay", 's', {"code", "symbol", "narrowSymbol", "name"}},
      {"currencySign", 's', {"standard", "accounting"}},
      {"unit", 's', {}},
      {"unitDisplay", 's', {"short", "narrow", "long"}},
      {"useGrouping", 'g', {"min2", "auto", "always", "true", "false"}},
      {"notation", 's', {"standard", "scientific", "engineering", "compact"}},
      {"compactDisplay", 's', {"short", "long"}},
      {"signDisplay", 's', {"auto", "never", "always", "exceptZero", "negative"}},
  };
  options.insert(options.end(), std::begin(kDigits), std::end(kDigits));

  return options;
}

const std::vector<IntlService> &services() {
  static const std::vector<IntlService> table = [] {
    std::vector<IntlOption> plural = {kMatcher, {"type", 's', {"cardinal", "ordinal"}}};
    plural.insert(plural.end(), std::begin(kDigits), std::end(kDigits));
    const std::vector<llvm::StringLiteral> widths = {"long", "short", "narrow"};

    return std::vector<IntlService>{
        {IntlKind::NumberFormat, "NumberFormat", TZ_INTL_NUMBER_FORMAT, numberOptions(), true},
        {IntlKind::Collator,
         "Collator",
         TZ_INTL_COLLATOR,
         {kMatcher,
          {"usage", 's', {"sort", "search"}},
          {"sensitivity", 's', {"base", "accent", "case", "variant"}},
          {"ignorePunctuation", 'b', {}},
          {"collation", 's', {}},
          {"numeric", 'b', {}},
          {"caseFirst", 's', {"upper", "lower", "false"}}},
         true},
        {IntlKind::PluralRules, "PluralRules", TZ_INTL_PLURAL_RULES, plural, false},
        {IntlKind::RelativeTimeFormat,
         "RelativeTimeFormat",
         TZ_INTL_RELATIVE_TIME_FORMAT,
         {kMatcher, kNumbering, {"style", 's', widths}, {"numeric", 's', {"always", "auto"}}},
         false},
        {IntlKind::ListFormat,
         "ListFormat",
         TZ_INTL_LIST_FORMAT,
         {kMatcher, {"type", 's', {"conjunction", "disjunction", "unit"}}, {"style", 's', widths}},
         false},
        {IntlKind::DisplayNames,
         "DisplayNames",
         TZ_INTL_DISPLAY_NAMES,
         {kMatcher,
          {"style", 's', widths},
          {"type", 's', {"language", "region", "script", "currency", "calendar", "dateTimeField"}},
          {"fallback", 's', {"code", "none"}},
          {"languageDisplay", 's', {"dialect", "standard"}}},
         false},
        {IntlKind::Segmenter,
         "Segmenter",
         TZ_INTL_SEGMENTER,
         {kMatcher, {"granularity", 's', {"grapheme", "word", "sentence"}}},
         false},
    };
  }();

  return table;
}

} // namespace tz
