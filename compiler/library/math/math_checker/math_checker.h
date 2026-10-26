#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class MathChecker {
public:
  explicit MathChecker(Checker &checker);

  Expr *checkMathCall(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkMinMaxSpread(CallExpr *call, const Type *hint);
  Expr *numericArg(Expr *arg, const Type *hint = nullptr);

private:
  Checker &checker_;
};

}
