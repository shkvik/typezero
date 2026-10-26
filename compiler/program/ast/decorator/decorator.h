#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

struct Decorator {
  llvm::SMLoc loc;
  Expr *expr;
};

}
