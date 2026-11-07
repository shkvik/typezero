#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class MethodLiteralChecker {
public:
  explicit MethodLiteralChecker(Checker &owner);

  Expr *checkMethodLiteral(ObjectLitExpr *lit, const Type *target);
  bool implementsLiteral(ObjectLitExpr *lit, const Type *target);

private:
  Checker &checker;
};

}
