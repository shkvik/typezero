#pragma once

#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/stmt/stmt.h"

namespace tz {

struct DoWhileStmt : Stmt {
  DoWhileStmt(llvm::SMLoc loc, Stmt *body, Expr *cond)
      : Stmt(Kind::DoWhile, loc), body(body), cond(cond) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::DoWhile; }

  Stmt *body;
  Expr *cond;
};

} // namespace tz
