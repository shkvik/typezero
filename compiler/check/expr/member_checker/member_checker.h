#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Members and indices: o.x, a[i], and where a field is.
class MemberChecker {
public:
  explicit MemberChecker(Checker &checker);

  Expr *checkMember(MemberExpr *member, const Type *expected);
  Expr *checkIndex(IndexExpr *index);
  // A position or count argument: any integer, computed as long.
  Expr *checkPosition(Expr *expr);

private:
  Checker &checker_;
};

} // namespace tz
