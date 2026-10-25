#pragma once

#include "compiler/program/ast/operators/operators.h"
#include "compiler/program/ast/expr/expr.h"

namespace tz {

// `target = value`, or a compound assignment such as `target += value`.
class AssignExpr : public Expr {
public:
  AssignExpr(llvm::SMLoc loc, std::optional<BinaryOp> op, Expr *target, Expr *value)
      : Expr(Kind::Assign, loc), op(op), target(target), value(value) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Assign; }

  std::optional<BinaryOp> op;
  Expr *target;
  Expr *value;
  // As in BinaryExpr.
  llvm::StringRef source;
};

} // namespace tz
