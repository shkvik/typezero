#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

// `await value` in an async function or in top-level code (docs/async.md):
// the code waits for the Promise `value` to settle and goes on with its
// value, or throws its error; a value that is no Promise it just gives back,
// after a turn of the microtask queue, as JavaScript does.
class AwaitExpr : public Expr {
public:
  AwaitExpr(llvm::SMLoc loc, Expr *value) : Expr(Kind::Await, loc), value(value) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Await; }

  Expr *value;
};

} // namespace tz
