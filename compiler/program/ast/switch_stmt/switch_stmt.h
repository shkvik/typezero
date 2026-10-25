#pragma once

#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/stmt/stmt.h"
#include "compiler/program/ast/switch_case/switch_case.h"

namespace tz {

// `switch (discriminant) { case a: ... default: ... }`. The discriminant is
// evaluated once and compared with the tests in order, with `==`; control
// falls through from one case into the next.
struct SwitchStmt : Stmt {
  SwitchStmt(llvm::SMLoc loc, Expr *discriminant)
      : Stmt(Kind::Switch, loc), discriminant(discriminant) {}
  static bool classof(const Stmt *s) { return s->kind() == Kind::Switch; }

  Expr *discriminant;
  std::vector<SwitchCase> cases;
  // Set by Checker: the cases handle every value of the literal type the switch
  // tests, so one of them always runs, as with a default.
  bool exhaustive = false;
};

} // namespace tz
