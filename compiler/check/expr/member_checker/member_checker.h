#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Members and indices: o.x, a[i], and where a field is.
class MemberChecker {
public:
  explicit MemberChecker(Sema &sema);

  Expr *checkMember(MemberExpr *member, const Type *expected);
  Expr *checkIndex(IndexExpr *index);
  // A position or count argument: any integer, computed as long.
  Expr *checkPosition(Expr *expr);

private:
  Sema &sema_;
};

} // namespace tz
