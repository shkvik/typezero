#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

// `a?.b.c`, `a?.[i]`, `s?.trim()`: the member accesses, indexing and calls
// after `?.` form the chain, and if the object before any `?.` in it is
// undefined, the whole chain is undefined and the rest is not evaluated. Checker
// marks those objects with CastExpr::shortCircuit.
class OptionalChainExpr : public Expr {
public:
  OptionalChainExpr(llvm::SMLoc loc, Expr *chain) : Expr(Kind::OptionalChain, loc), chain(chain) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::OptionalChain; }

  Expr *chain;
  // Set by Checker: some object in the chain may be undefined.
  bool mayShortCircuit = false;
};

} // namespace tz
