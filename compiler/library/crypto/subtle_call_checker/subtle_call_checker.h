#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class SubtleCallChecker {
public:
  explicit SubtleCallChecker(Checker &checker);

  Expr *checkSubtleCall(CallExpr *call, MemberExpr *member, const Type *expected);

private:
  Checker &checker_;
};

}
