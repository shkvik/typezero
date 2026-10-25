#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

struct EnumMember {
  llvm::StringRef name;
  llvm::SMLoc loc;
  Expr *init = nullptr;
  // Set by Sema: the value, a number or a string.
  bool isString = false;
  int32_t number = 0;
  std::u16string text;
};

} // namespace tz
