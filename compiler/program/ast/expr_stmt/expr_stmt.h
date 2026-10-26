#pragma once

#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/stmt/stmt.h"

namespace tz {

struct ExprStmt : Stmt {
  ExprStmt(llvm::SMLoc loc, Expr *expr) : Stmt(Kind::Expr, loc), expr(expr) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Expr; }

  Expr *expr;
};

}
