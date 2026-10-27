#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

struct EnumMember {
  llvm::StringRef name;
  llvm::SMLoc loc;
  Expr *init = nullptr;
  bool isString = false;
  int64_t number = 0;
  std::u16string text;
};

}
