#pragma once

#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/stmt/stmt.h"
#include "compiler/program/ast/switch_case/switch_case.h"

namespace tz {

struct SwitchStmt : Stmt {
  SwitchStmt(llvm::SMLoc loc, Expr *discriminant)
      : Stmt(Kind::Switch, loc), discriminant(discriminant) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Switch; }

  Expr *discriminant;
  std::vector<SwitchCase> cases;
  bool exhaustive = false;
};

}
