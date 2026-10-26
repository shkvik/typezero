#pragma once

#include "compiler/check/checker/checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>

namespace tz {

struct IntlOption {
  llvm::StringLiteral name;
  char kind;
  std::vector<llvm::StringLiteral> values;
};

inline const IntlOption kMatcher = {"localeMatcher", 's', {"lookup", "best fit"}};

inline const IntlOption kNumbering = {"numberingSystem", 's', {}};

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

struct IntlService {
  IntlKind kind;
  llvm::StringLiteral name;
  int runtimeKind;
  std::vector<IntlOption> options;
  bool callable;
};

const std::vector<IntlService> &services();

}
