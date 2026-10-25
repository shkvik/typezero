#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

// `a, b, c`: evaluates each in order; the value is the last one's.
class SequenceExpr : public Expr {
public:
  SequenceExpr(llvm::SMLoc loc, std::vector<Expr *> exprs)
      : Expr(Kind::Sequence, loc), exprs(std::move(exprs)) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Sequence; }

  std::vector<Expr *> exprs;
};

} // namespace tz
