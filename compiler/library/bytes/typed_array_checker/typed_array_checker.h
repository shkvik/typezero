#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class TypedArrayChecker {
public:
  explicit TypedArrayChecker(Checker &checker);

  Expr *checkTypedArrayMethod(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkDataViewMethod(CallExpr *call, MemberExpr *member);

private:
  Checker &checker_;
};

}
