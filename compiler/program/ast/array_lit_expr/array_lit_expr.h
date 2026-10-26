#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

class ArrayLitExpr : public Expr {
public:
  struct Element {
    Expr *expr;
    bool spread;
    bool hole = false;
  };

  explicit ArrayLitExpr(llvm::SMLoc loc) : Expr(Kind::ArrayLit, loc) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::ArrayLit; }

  std::vector<Element> elements;
};

}
