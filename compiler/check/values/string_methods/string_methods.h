#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The methods of strings.
class StringMethods {
public:
  explicit StringMethods(Checker &checker);

  Expr *checkStringMethod(CallExpr *call, MemberExpr *member);

private:
  Checker &checker_;
};

} // namespace tz
