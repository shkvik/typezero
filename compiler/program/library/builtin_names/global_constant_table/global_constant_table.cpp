#include "compiler/program/library/builtin_names/global_constant_table/global_constant_table.h"
#include <limits>

namespace tz {

namespace {

const GlobalConstantInfo kRows[] = {
    {
        .owner = GlobalName::MATH,
        .name = "E",
        .value = 2.718281828459045,
    },
    {
        .owner = GlobalName::MATH,
        .name = "LN10",
        .value = 2.302585092994046,
    },
    {
        .owner = GlobalName::MATH,
        .name = "LN2",
        .value = 0.6931471805599453,
    },
    {
        .owner = GlobalName::MATH,
        .name = "LOG10E",
        .value = 0.4342944819032518,
    },
    {
        .owner = GlobalName::MATH,
        .name = "LOG2E",
        .value = 1.4426950408889634,
    },
    {
        .owner = GlobalName::MATH,
        .name = "PI",
        .value = 3.141592653589793,
    },
    {
        .owner = GlobalName::MATH,
        .name = "SQRT1_2",
        .value = 0.7071067811865476,
    },
    {
        .owner = GlobalName::MATH,
        .name = "SQRT2",
        .value = 1.4142135623730951,
    },
    {
        .owner = GlobalName::NUMBER,
        .name = "MAX_SAFE_INTEGER",
        .value = 9007199254740991.0,
    },
    {
        .owner = GlobalName::NUMBER,
        .name = "MIN_SAFE_INTEGER",
        .value = -9007199254740991.0,
    },
    {
        .owner = GlobalName::NUMBER,
        .name = "EPSILON",
        .value = 2.220446049250313e-16,
    },
    {
        .owner = GlobalName::NUMBER,
        .name = "MAX_VALUE",
        .value = std::numeric_limits<double>::max(),
    },
    {
        .owner = GlobalName::NUMBER,
        .name = "MIN_VALUE",
        .value = std::numeric_limits<double>::denorm_min(),
    },
};

}

std::optional<double> GlobalConstantTable::find(GlobalName owner, llvm::StringRef name) {
  for (const GlobalConstantInfo &info : kRows) {
    if (info.owner == owner && info.name == name) {
      return info.value;
    }
  }

  return std::nullopt;
}

}
