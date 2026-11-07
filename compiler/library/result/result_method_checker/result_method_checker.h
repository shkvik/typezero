#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ResultMethodChecker {
public:
  explicit ResultMethodChecker(Checker &owner);

  Expr *checkResultMethod(CallExpr *call, MemberExpr *member, const Type *expected);

private:
  static const Type *halfParam(const Type *half);
  static bool givesType(const Type *half);

  Checker &checker;
};

}
