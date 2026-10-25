#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The methods of typed arrays and DataView.
class TypedArrayChecker {
public:
  explicit TypedArrayChecker(Sema &sema);

  Expr *checkTypedArrayMethod(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkDataViewMethod(CallExpr *call, MemberExpr *member);

private:
  Sema &sema_;
};

} // namespace tz
