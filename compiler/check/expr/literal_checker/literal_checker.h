#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Number literals and template literals.
class LiteralChecker {
public:
  explicit LiteralChecker(Sema &sema);

  Expr *checkIntLit(IntLitExpr *lit, const Type *expected, bool negated);
  // A template literal; where a template literal type is expected, it has
  // the type its parts make (`` `on${string}` ``).
  Expr *checkTemplate(TemplateExpr *tmpl, const Type *expected = nullptr);

private:
  Sema &sema_;
};

} // namespace tz
