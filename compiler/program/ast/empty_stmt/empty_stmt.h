#pragma once

#include "compiler/program/ast/stmt/stmt.h"

namespace tz {

struct EmptyStmt : Stmt {
  explicit EmptyStmt(llvm::SMLoc loc) : Stmt(Kind::Empty, loc) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Empty; }
};

} // namespace tz
