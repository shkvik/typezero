#pragma once

#include "compiler/program/ast/expr/expr.h"

namespace tz {

// `a${x}b${y}c`: quasis are the texts "a", "b", "c" around the expressions.
class TemplateExpr : public Expr {
public:
  explicit TemplateExpr(llvm::SMLoc loc) : Expr(Kind::Template, loc) {}
  static bool classof(const Expr *e) { return e->kind() == Kind::Template; }

  std::vector<std::u16string> quasis; // always exprs.size() + 1 entries
  std::vector<Expr *> exprs;
};

} // namespace tz
