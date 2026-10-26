#pragma once

#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/stmt/stmt.h"
#include "compiler/program/ast/var_declarator/var_declarator.h"

namespace tz {

struct ForOfStmt : Stmt {
  explicit ForOfStmt(llvm::SMLoc loc) : Stmt(Kind::ForOf, loc) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::ForOf; }

  bool isConst = false;
  bool keys = false;
  bool isAwait = false;
  VarDeclarator binding;
  Expr *iterable = nullptr;
  Stmt *body = nullptr;
  bool borrowElements = false;
};

}
