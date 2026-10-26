#pragma once

#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/stmt/stmt.h"

namespace tz {

struct WhileStmt : Stmt {
  WhileStmt(llvm::SMLoc loc, Expr *cond, Stmt *body)
      : Stmt(Kind::While, loc), cond(cond), body(body) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::While; }

  Expr *cond;
  Stmt *body;
};

}
