#pragma once

#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/stmt/stmt.h"

namespace tz {

struct ForStmt : Stmt {
  explicit ForStmt(llvm::SMLoc loc) : Stmt(Kind::For, loc) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::For; }

  Stmt *init = nullptr; // VarDeclStmt or ExprStmt
  Expr *cond = nullptr;
  Expr *step = nullptr;
  Stmt *body = nullptr;
};

} // namespace tz
