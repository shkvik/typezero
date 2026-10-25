#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The methods of arrays.
class ArrayMethods {
public:
  explicit ArrayMethods(Sema &sema);

  Expr *checkArrayMethod(CallExpr *call, MemberExpr *member, const Type *expected);

private:
  Sema &sema_;
};

} // namespace tz
