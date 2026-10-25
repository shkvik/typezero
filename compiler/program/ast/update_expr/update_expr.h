#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

// `++x`, `x++`, `--x`, `x--`.
class UpdateExpr : public Expr {
public:
  UpdateExpr(llvm::SMLoc loc, bool increment, bool prefix, Expr *target)
      : Expr(Kind::Update, loc), increment(increment), prefix(prefix), target(target) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Update; }

  bool increment;
  bool prefix;
  Expr *target;
};

} // namespace tz
