#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

class BoolLitExpr : public Expr {
public:
  BoolLitExpr(llvm::SMLoc loc, bool value) : Expr(Kind::BoolLit, loc), value(value) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::BoolLit; }

  bool value;
};

} // namespace tz
