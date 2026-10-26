#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ResultMethodChecker {
public:
  explicit ResultMethodChecker(Checker &checker);

  Expr *checkResultMethod(CallExpr *call, MemberExpr *member, const Type *expected);

private:
  Checker &checker_;
};

}
