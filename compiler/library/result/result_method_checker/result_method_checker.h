#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The methods of Result.
class ResultMethodChecker {
public:
  explicit ResultMethodChecker(Checker &checker);

  // `r.map(f)` and the other methods, whose receiver is checked.
  Expr *checkResultMethod(CallExpr *call, MemberExpr *member, const Type *expected);

private:
  Checker &checker_;
};

} // namespace tz
