#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

class IntLitExpr : public Expr {
public:
  IntLitExpr(llvm::SMLoc loc, uint64_t value) : Expr(Kind::IntLit, loc), value(value) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::IntLit; }

  uint64_t value;
  llvm::StringRef text;
};

}
