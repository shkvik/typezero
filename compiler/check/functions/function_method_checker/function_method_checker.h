#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class FunctionMethodChecker {
public:
  explicit FunctionMethodChecker(Checker &checker);

  Expr *checkThisCall(CallExpr *call, MemberExpr *member);
  Expr *checkListMethod(CallExpr *call, MemberExpr *member);

private:
  Checker &checker_;
};

}
