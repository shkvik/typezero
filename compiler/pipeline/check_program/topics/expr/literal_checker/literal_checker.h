#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/expr/literal_checker/literal_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class LiteralChecker {
public:
  using ResultIntLiteralTyped = Result<Expr *, LiteralCheckerCheckIntLitError>;

  explicit LiteralChecker(CheckState &owner);

  Expr *checkTemplate(TemplateExpr *tmpl, const Type *expected = nullptr);
  ResultIntLiteralTyped checkIntLit(IntLitExpr *lit, const Type *expected, bool negated);

private:
  static bool fitsIn(const IntLitExpr *lit, const Type *type, bool negated);

  CheckState &state;
};

}
