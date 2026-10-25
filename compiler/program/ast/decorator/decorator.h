#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

// `@Log` or `@Table("users")` before a class, a member of one or a parameter
// of a method or a constructor (docs/decorators.md).
struct Decorator {
  llvm::SMLoc loc; // of the `@`
  Expr *expr;      // `Log`, `Table("users")`, `(make())`
};

} // namespace tz
