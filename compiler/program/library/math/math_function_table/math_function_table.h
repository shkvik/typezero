#pragma once

#include "compiler/program/ast/builtin/builtin.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

enum class MathFunctionForm {
  ROUNDING,
  FLOAT,
  SAME,
  MIN_MAX,
  POW,
  INT32,
  CLAMP,
};

struct MathFunctionInfo {
  llvm::StringLiteral name;
  Builtin builtin;
  MathFunctionForm form;
  unsigned minArgs;
  unsigned maxArgs;
};

class MathFunctionTable {
public:
  static constexpr unsigned kAnyArgs = ~0u;

  static const MathFunctionInfo *find(llvm::StringRef name);
};

}
