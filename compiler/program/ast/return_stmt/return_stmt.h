#pragma once

#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/stmt/stmt.h"

namespace tz {

struct ReturnStmt : Stmt {
  ReturnStmt(llvm::SMLoc loc, Expr *value) : Stmt(Kind::Return, loc), value(value) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Return; }

  Expr *value; // may be null
};

} // namespace tz
