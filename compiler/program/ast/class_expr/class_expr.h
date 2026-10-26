#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

struct ClassDecl;

class ClassExpr : public Expr {
public:
  ClassExpr(llvm::SMLoc loc, ClassDecl *decl) : Expr(Kind::Class, loc), decl(decl) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Class; }

  ClassDecl *decl;
};

}
