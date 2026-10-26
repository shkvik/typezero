#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

class StringLitExpr : public Expr {
public:
  StringLitExpr(llvm::SMLoc loc, std::u16string value)
      : Expr(Kind::StringLit, loc), value(std::move(value)) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::StringLit; }

  std::u16string value;
};

}
