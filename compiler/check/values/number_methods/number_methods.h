#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The methods of numbers and the statics of Number and String.
class NumberMethods {
public:
  explicit NumberMethods(Checker &checker);

  // String.fromCharCode, Number.isNaN...
  Expr *checkStaticCall(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkNumberMethod(CallExpr *call, MemberExpr *member);

private:
  Checker &checker_;
};

} // namespace tz
