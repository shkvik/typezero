#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class NumberMethods {
public:
  explicit NumberMethods(Checker &checker);

  Expr *checkStaticCall(CallExpr *call, MemberExpr *member);
  Expr *checkNumberMethod(CallExpr *call, MemberExpr *member);
  Expr *checkConversion(CallExpr *call, MemberExpr *member);

private:
  Checker &checker_;
};

}
