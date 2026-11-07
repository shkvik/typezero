#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/check/expr/literal_checker/literal_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class LiteralChecker {
public:
  using ResultIntLiteralTyped = Result<Expr *, LiteralCheckerCheckIntLitError>;

  explicit LiteralChecker(Checker &owner);

  Expr *checkTemplate(TemplateExpr *tmpl, const Type *expected = nullptr);
  ResultIntLiteralTyped checkIntLit(IntLitExpr *lit, const Type *expected, bool negated);

private:
  static bool fitsIn(const IntLitExpr *lit, const Type *type, bool negated);

  Checker &checker;
};

}
