#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

class AwaitExpr : public Expr {
public:
  AwaitExpr(llvm::SMLoc loc, Expr *value) : Expr(Kind::Await, loc), value(value) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Await; }

  Expr *value;
};

}
