#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class MemberChecker {
public:
  explicit MemberChecker(Checker &owner);

  Expr *checkMember(MemberExpr *member, const Type *expected);
  Expr *checkIndex(IndexExpr *index);
  Expr *checkPosition(Expr *expr);

private:
  Checker &checker;
};

}
