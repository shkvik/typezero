#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The methods of strings.
class StringMethods {
public:
  explicit StringMethods(Sema &sema);

  Expr *checkStringMethod(CallExpr *call, MemberExpr *member);

private:
  Sema &sema_;
};

} // namespace tz
