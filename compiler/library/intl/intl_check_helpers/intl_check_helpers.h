#pragma once

#include "compiler/check/checker/checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>

namespace tz {

// Functions that several classes made from sema_intl.cpp use.

// An option: its type ('s' a string, 'n' a number, 'b' a boolean, 'g' a
// string or a boolean), and the values a string may have (none: any).
struct IntlOption {
  llvm::StringLiteral name;
  char kind;
  std::vector<llvm::StringLiteral> values;
};

inline const IntlOption kMatcher = {"localeMatcher", 's', {"lookup", "best fit"}};

inline const IntlOption kNumbering = {"numberingSystem", 's', {}};

// The digit options of NumberFormat and PluralRules.
inline const IntlOption kDigits[] = {
    {"minimumIntegerDigits", 'n', {}},
    {"minimumFractionDigits", 'n', {}},
    {"maximumFractionDigits", 'n', {}},
    {"minimumSignificantDigits", 'n', {}},
    {"maximumSignificantDigits", 'n', {}},
    {"roundingIncrement", 'n', {}},
    {"roundingMode",
     's',
     {"ceil", "floor", "expand", "trunc", "halfCeil", "halfFloor", "halfExpand", "halfTrunc",
      "halfEven"}},
    {"roundingPriority", 's', {"auto", "morePrecision", "lessPrecision"}},
    {"trailingZeroDisplay", 's', {"auto", "stripIfInteger"}},
};

std::vector<IntlOption> numberOptions();

// A kind of object of Intl: what JavaScript calls it, what its options are,
// and whether it can be made without `new`.
struct IntlService {
  IntlKind kind;
  llvm::StringLiteral name;
  int runtimeKind; // TZ_INTL_*
  std::vector<IntlOption> options;
  bool callable;
};

const std::vector<IntlService> &services();

} // namespace tz
