#pragma once

#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/stmt/stmt.h"

namespace tz {

struct IfStmt : Stmt {
  IfStmt(llvm::SMLoc loc, Expr *cond, Stmt *thenStmt, Stmt *elseStmt)
      : Stmt(Kind::If, loc), cond(cond), thenStmt(thenStmt), elseStmt(elseStmt) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::If; }

  Expr *cond;
  Stmt *thenStmt;
  Stmt *elseStmt; // may be null
};

} // namespace tz
