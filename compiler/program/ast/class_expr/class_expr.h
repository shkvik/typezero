#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

struct ClassDecl;

// `class extends Base { ... }` as an expression (docs/decorators.md, section
// 5): its value is the class, which Sema makes when it checks the
// expression, once for each copy of a generic function that has it. Its
// static fields get their values each time it is evaluated.
class ClassExpr : public Expr {
public:
  ClassExpr(llvm::SMLoc loc, ClassDecl *decl) : Expr(Kind::Class, loc), decl(decl) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Class; }

  ClassDecl *decl;
};

} // namespace tz
