#pragma once

#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/operators/operators.h"

namespace tz {

class BinaryExpr : public Expr {
public:
  BinaryExpr(llvm::SMLoc loc, BinaryOp op, Expr *lhs, Expr *rhs)
      : Expr(Kind::Binary, loc), op(op), lhs(lhs), rhs(rhs) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Binary; }

  BinaryOp op;
  Expr *lhs;
  Expr *rhs;
  llvm::StringRef source;
};

}
