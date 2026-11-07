#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/check/expr/literal_checker/literal_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class LiteralChecker {
public:
  explicit LiteralChecker(Checker &owner);

  Expr *checkIntLit(IntLitExpr *lit, const Type *expected, bool negated);
  Expr *checkTemplate(TemplateExpr *tmpl, const Type *expected = nullptr);

private:
  using ResultIntLiteralFits = Result<const Type *, LiteralCheckerIntFitsError>;

  static bool fitsIn(const IntLitExpr *lit, const Type *type, bool negated);
  static ResultIntLiteralFits intFits(const IntLitExpr *lit, const Type *type, bool negated);

  Checker &checker;
};

}
