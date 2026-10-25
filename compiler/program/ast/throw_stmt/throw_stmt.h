#pragma once

#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/stmt/stmt.h"

namespace tz {

// `throw value`: only an Error (or an instance of a class derived from it)
// can be thrown (docs/errors.md).
struct ThrowStmt : Stmt {
  ThrowStmt(llvm::SMLoc loc, Expr *value) : Stmt(Kind::Throw, loc), value(value) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Throw; }

  Expr *value;
};

} // namespace tz
