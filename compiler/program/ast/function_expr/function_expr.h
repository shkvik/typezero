#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

struct FunctionDecl;

class FunctionExpr : public Expr {
public:
  FunctionExpr(llvm::SMLoc loc, FunctionDecl *fn) : Expr(Kind::Function, loc), fn(fn) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Function; }

  FunctionDecl *fn;
};

}
