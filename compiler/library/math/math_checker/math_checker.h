#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The functions of Math.
class MathChecker {
public:
  explicit MathChecker(Sema &sema);

  Expr *checkMathCall(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkMinMaxSpread(CallExpr *call, const Type *hint);
  // An argument that must be a number; reports anything else.
  Expr *numericArg(Expr *arg, const Type *hint = nullptr);

private:
  Sema &sema_;
};

} // namespace tz
