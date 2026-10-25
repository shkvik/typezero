#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The calls of crypto.subtle: their algorithms checked where they are written.
class SubtleCallChecker {
public:
  explicit SubtleCallChecker(Sema &sema);

  Expr *checkSubtleCall(CallExpr *call, MemberExpr *member, const Type *expected);

private:
  Sema &sema_;
};

} // namespace tz
