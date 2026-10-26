#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

class TemplateExpr : public Expr {
public:
  explicit TemplateExpr(llvm::SMLoc loc) : Expr(Kind::Template, loc) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Template; }

  std::vector<std::u16string> quasis;
  std::vector<Expr *> exprs;
};

}
