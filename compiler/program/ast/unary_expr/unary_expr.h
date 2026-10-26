#pragma once

#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/operators/operators.h"

namespace tz {

class UnaryExpr : public Expr {
public:
  UnaryExpr(llvm::SMLoc loc, UnaryOp op, Expr *operand)
      : Expr(Kind::Unary, loc), op(op), operand(operand) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Unary; }

  UnaryOp op;
  Expr *operand;
};

}
