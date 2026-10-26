#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

class OptionalChainExpr : public Expr {
public:
  OptionalChainExpr(llvm::SMLoc loc, Expr *chain) : Expr(Kind::OptionalChain, loc), chain(chain) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::OptionalChain; }

  Expr *chain;
  bool mayShortCircuit = false;
};

}
