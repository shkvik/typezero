#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Number literals and template literals.
class LiteralChecker {
public:
  explicit LiteralChecker(Checker &checker);

  Expr *checkIntLit(IntLitExpr *lit, const Type *expected, bool negated);
  // A template literal; where a template literal type is expected, it has
  // the type its parts make (`` `on${string}` ``).
  Expr *checkTemplate(TemplateExpr *tmpl, const Type *expected = nullptr);

private:
  Checker &checker_;
};

} // namespace tz
