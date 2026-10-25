#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

class ConditionalExpr : public Expr {
public:
  ConditionalExpr(llvm::SMLoc loc, Expr *cond, Expr *thenExpr, Expr *elseExpr)
      : Expr(Kind::Conditional, loc), cond(cond), thenExpr(thenExpr), elseExpr(elseExpr) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Conditional; }

  Expr *cond;
  Expr *thenExpr;
  Expr *elseExpr;
};

} // namespace tz
