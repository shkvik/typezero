#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The methods of numbers and the statics of Number and String.
class NumberMethods {
public:
  explicit NumberMethods(Sema &sema);

  // String.fromCharCode, Number.isNaN...
  Expr *checkStaticCall(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkNumberMethod(CallExpr *call, MemberExpr *member);

private:
  Sema &sema_;
};

} // namespace tz
