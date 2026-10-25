#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

struct FunctionDecl;

// An arrow function or a function expression: a function value, which
// captures the variables around it that its body uses (see
// FunctionDecl::captures).
class FunctionExpr : public Expr {
public:
  FunctionExpr(llvm::SMLoc loc, FunctionDecl *fn) : Expr(Kind::Function, loc), fn(fn) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Function; }

  FunctionDecl *fn;
};

} // namespace tz
