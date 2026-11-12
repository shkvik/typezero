#include "compiler/program/library/regex/regex_flag_table/regex_flag_table.h"
#include "runtime/library/regex/regex/regex.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

const RegexFlagInfo kFlags[] = {
    {.name = "hasIndices", .bit = TZ_RE_HAS_INDICES},
    {.name = "global", .bit = TZ_RE_GLOBAL},
    {.name = "ignoreCase", .bit = TZ_RE_IGNORE_CASE},
    {.name = "multiline", .bit = TZ_RE_MULTILINE},
    {.name = "dotAll", .bit = TZ_RE_DOT_ALL},
    {.name = "unicode", .bit = TZ_RE_UNICODE},
    {.name = "unicodeSets", .bit = TZ_RE_UNICODE_SETS},
    {.name = "sticky", .bit = TZ_RE_STICKY},
};

constexpr llvm::StringLiteral kLegacyPrefix = "$";

const llvm::StringLiteral kLegacyStatics[] = {"input", "lastMatch", "lastParen", "leftContext",
                                              "rightContext"};

}

std::optional<uint32_t> RegexFlagTable::findBit(llvm::StringRef name) {
  for (const RegexFlagInfo &flag : kFlags) {
    if (flag.name == name) {
      return flag.bit;
    }
  }

  return std::nullopt;
}

bool RegexFlagTable::isLegacyStatic(llvm::StringRef name) {
  return name.starts_with(kLegacyPrefix) || llvm::is_contained(kLegacyStatics, name);
}

}
