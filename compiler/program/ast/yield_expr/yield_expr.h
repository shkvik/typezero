#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

class Type;

class YieldExpr : public Expr {
public:
  YieldExpr(llvm::SMLoc loc, Expr *value, bool delegate)
      : Expr(Kind::Yield, loc), value(value), delegate(delegate) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Yield; }

  Expr *value;
  bool delegate;
  bool discarded = false;
  const Type *element = nullptr;
};

}
