#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

class Type;

// `yield value` (or a bare `yield`) in a generator: gives the value to
// whoever iterates, and is what `next(x)` sends back, if anything.
// `yield* iterable` gives each value of an array, a string or another
// iterator in turn, and is what that iterator returns.
class YieldExpr : public Expr {
public:
  YieldExpr(llvm::SMLoc loc, Expr *value, bool delegate)
      : Expr(Kind::Yield, loc), value(value), delegate(delegate) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Yield; }

  Expr *value; // null for a bare `yield`
  bool delegate;
  // Set by Sema. The statement drops the value of the `yield`, so what
  // next() sends does not matter. For `yield*`, the type of the values the
  // iterable gives, which each become the generator's.
  bool discarded = false;
  const Type *element = nullptr;
};

} // namespace tz
