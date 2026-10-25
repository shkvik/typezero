#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The functions of Math.
class MathChecker {
public:
  explicit MathChecker(Checker &checker);

  Expr *checkMathCall(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkMinMaxSpread(CallExpr *call, const Type *hint);
  // An argument that must be a number; reports anything else.
  Expr *numericArg(Expr *arg, const Type *hint = nullptr);

private:
  Checker &checker_;
};

} // namespace tz
