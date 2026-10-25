#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

// `[a, b, ...c]`
class ArrayLitExpr : public Expr {
public:
  struct Element {
    Expr *expr;
    bool spread;       // `...expr`
    bool hole = false; // `[a, , b]`: allowed only in destructuring; expr is undefined
  };

  explicit ArrayLitExpr(llvm::SMLoc loc) : Expr(Kind::ArrayLit, loc) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::ArrayLit; }

  std::vector<Element> elements;
};

} // namespace tz
