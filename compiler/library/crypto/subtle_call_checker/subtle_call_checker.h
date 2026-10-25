#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The calls of crypto.subtle: their algorithms checked where they are written.
class SubtleCallChecker {
public:
  explicit SubtleCallChecker(Checker &checker);

  Expr *checkSubtleCall(CallExpr *call, MemberExpr *member, const Type *expected);

private:
  Checker &checker_;
};

} // namespace tz
