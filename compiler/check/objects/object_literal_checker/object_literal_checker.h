#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ObjectLiteralChecker {
public:
  explicit ObjectLiteralChecker(Checker &checker);

  Expr *checkObjectLit(ObjectLitExpr *lit, const Type *expected);
  Expr *checkSpreadLit(ObjectLitExpr *lit, const Type *target, llvm::ArrayRef<bool> named, bool ok);

private:
  Checker &checker_;
};

}
