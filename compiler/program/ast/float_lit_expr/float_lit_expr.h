#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

class FloatLitExpr : public Expr {
public:
  FloatLitExpr(llvm::SMLoc loc, double value) : Expr(Kind::FloatLit, loc), value(value) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::FloatLit; }

  double value;
  llvm::StringRef text;
};

}
