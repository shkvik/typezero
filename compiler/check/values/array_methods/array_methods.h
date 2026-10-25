#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The methods of arrays.
class ArrayMethods {
public:
  explicit ArrayMethods(Checker &checker);

  Expr *checkArrayMethod(CallExpr *call, MemberExpr *member, const Type *expected);

private:
  Checker &checker_;
};

} // namespace tz
