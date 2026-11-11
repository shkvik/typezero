#pragma once

#include "compiler/program/ast/builtin/builtin.h"
#include "llvm/ADT/StringRef.h"
#include <array>

namespace tz {

enum class DecimalArg {
  NONE,
  DECIMAL,
  PLACES,
  DIGITS,
  BASE,
  POWER,
  ROUNDING,
};

enum class DecimalResult {
  DECIMAL,
  STRING,
  BOOL,
  INT,
  DECIMAL_LIST,
};

struct DecimalMethodInfo {
  llvm::StringLiteral name;
  Builtin builtin;
  unsigned minArgs;
  unsigned maxArgs;
  std::array<DecimalArg, 3> args;
  DecimalResult result;
};

class DecimalMethodTable {
public:
  static const DecimalMethodInfo *find(llvm::StringRef name);
};

}
