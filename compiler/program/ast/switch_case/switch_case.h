#pragma once

#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/stmt/stmt.h"

namespace tz {

class Type;

// `case test:` (or `default:`, which has no test) and the statements after it.
struct SwitchCase {
  llvm::SMLoc loc;
  Expr *test = nullptr;
  std::vector<Stmt *> body;
  // Set by Sema: the type in which the discriminant and the test compare, as
  // with `==`. Null when the test is `undefined`.
  const Type *compareType = nullptr;
};

} // namespace tz
