#pragma once

#include "compiler/union/union/union.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

enum class FunctionSyntaxKind {
  REGULAR_FUNCTION,
  ARROW_FUNCTION,
};

struct RegularFunction {
  static constexpr FunctionSyntaxKind kind = FunctionSyntaxKind::REGULAR_FUNCTION;
  llvm::StringRef ownName;
};

struct ArrowFunction {
  static constexpr FunctionSyntaxKind kind = FunctionSyntaxKind::ARROW_FUNCTION;
  bool concise = false;
};

using FunctionSyntax = Union<RegularFunction, ArrowFunction>;

enum class FunctionPlace {
  DECLARED,
  EXPRESSION,
  LIFTED,
};

enum class FunctionBodySource {
  WRITTEN,
  EXTERNAL,
  INTRINSIC,
};

}
