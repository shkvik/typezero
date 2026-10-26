#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

class UndefinedExpr : public Expr {
public:
  explicit UndefinedExpr(llvm::SMLoc loc) : Expr(Kind::Undefined, loc) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Undefined; }
};

}
