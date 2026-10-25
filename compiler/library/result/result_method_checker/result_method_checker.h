#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The methods of Result.
class ResultMethodChecker {
public:
  explicit ResultMethodChecker(Sema &sema);

  // `r.map(f)` and the other methods, whose receiver is checked.
  Expr *checkResultMethod(CallExpr *call, MemberExpr *member, const Type *expected);

private:
  Sema &sema_;
};

} // namespace tz
