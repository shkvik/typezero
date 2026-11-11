#include "compiler/program/library/decimal/decimal_method_table/decimal_method_table.h"

namespace tz {

namespace {

const DecimalMethodInfo kDecimalMethods[] = {
    {
        .name = "div",
        .builtin = Builtin::DecDiv,
        .minArgs = 1,
        .maxArgs = 3,
        .args = {{DecimalArg::DECIMAL, DecimalArg::PLACES, DecimalArg::ROUNDING}},
        .result = DecimalResult::DECIMAL,
    },
    {
        .name = "round",
        .builtin = Builtin::DecRound,
        .minArgs = 0,
        .maxArgs = 2,
        .args = {{DecimalArg::PLACES, DecimalArg::ROUNDING, DecimalArg::NONE}},
        .result = DecimalResult::DECIMAL,
    },
    {
        .name = "toFixed",
        .builtin = Builtin::DecToFixed,
        .minArgs = 0,
        .maxArgs = 2,
        .args = {{DecimalArg::PLACES, DecimalArg::ROUNDING, DecimalArg::NONE}},
        .result = DecimalResult::STRING,
    },
    {
        .name = "toFormat",
        .builtin = Builtin::DecToFormat,
        .minArgs = 0,
        .maxArgs = 2,
        .args = {{DecimalArg::PLACES, DecimalArg::ROUNDING, DecimalArg::NONE}},
        .result = DecimalResult::STRING,
    },
    {
        .name = "toExponential",
        .builtin = Builtin::DecToExponential,
        .minArgs = 0,
        .maxArgs = 2,
        .args = {{DecimalArg::PLACES, DecimalArg::ROUNDING, DecimalArg::NONE}},
        .result = DecimalResult::STRING,
    },
    {
        .name = "toPrecision",
        .builtin = Builtin::DecToPrecision,
        .minArgs = 0,
        .maxArgs = 2,
        .args = {{DecimalArg::DIGITS, DecimalArg::ROUNDING, DecimalArg::NONE}},
        .result = DecimalResult::STRING,
    },
    {
        .name = "toString",
        .builtin = Builtin::DecToString,
        .minArgs = 0,
        .maxArgs = 1,
        .args = {{DecimalArg::BASE, DecimalArg::NONE, DecimalArg::NONE}},
        .result = DecimalResult::STRING,
    },
    {
        .name = "sqrt",
        .builtin = Builtin::DecSqrt,
        .minArgs = 0,
        .maxArgs = 0,
        .args = {{DecimalArg::NONE, DecimalArg::NONE, DecimalArg::NONE}},
        .result = DecimalResult::DECIMAL,
    },
    {
        .name = "abs",
        .builtin = Builtin::DecAbs,
        .minArgs = 0,
        .maxArgs = 0,
        .args = {{DecimalArg::NONE, DecimalArg::NONE, DecimalArg::NONE}},
        .result = DecimalResult::DECIMAL,
    },
    {
        .name = "isInteger",
        .builtin = Builtin::DecIsInteger,
        .minArgs = 0,
        .maxArgs = 0,
        .args = {{DecimalArg::NONE, DecimalArg::NONE, DecimalArg::NONE}},
        .result = DecimalResult::BOOL,
    },
    {
        .name = "decimalPlaces",
        .builtin = Builtin::DecDecimalPlaces,
        .minArgs = 0,
        .maxArgs = 0,
        .args = {{DecimalArg::NONE, DecimalArg::NONE, DecimalArg::NONE}},
        .result = DecimalResult::INT,
    },
    {
        .name = "shiftedBy",
        .builtin = Builtin::DecShiftedBy,
        .minArgs = 1,
        .maxArgs = 1,
        .args = {{DecimalArg::POWER, DecimalArg::NONE, DecimalArg::NONE}},
        .result = DecimalResult::DECIMAL,
    },
    {
        .name = "toFraction",
        .builtin = Builtin::DecToFraction,
        .minArgs = 0,
        .maxArgs = 1,
        .args = {{DecimalArg::DECIMAL, DecimalArg::NONE, DecimalArg::NONE}},
        .result = DecimalResult::DECIMAL_LIST,
    },
};

}

const DecimalMethodInfo *DecimalMethodTable::find(llvm::StringRef name) {
  for (const DecimalMethodInfo &info : kDecimalMethods) {
    if (info.name == name) {
      return &info;
    }
  }

  return nullptr;
}

}
