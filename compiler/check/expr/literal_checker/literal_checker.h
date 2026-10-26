#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class LiteralChecker {
public:
  explicit LiteralChecker(Checker &checker);

  Expr *checkIntLit(IntLitExpr *lit, const Type *expected, bool negated);
  Expr *checkTemplate(TemplateExpr *tmpl, const Type *expected = nullptr);

private:
  Checker &checker_;
};

}
