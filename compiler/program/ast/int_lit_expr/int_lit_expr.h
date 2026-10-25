#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

class IntLitExpr : public Expr {
public:
  IntLitExpr(llvm::SMLoc loc, uint64_t value) : Expr(Kind::IntLit, loc), value(value) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::IntLit; }

  // The magnitude; a leading minus is a separate UnaryExpr.
  uint64_t value;
  // The literal as written, from which a decimal reads its exact value;
  // empty for one the compiler made.
  llvm::StringRef text;
};

} // namespace tz
