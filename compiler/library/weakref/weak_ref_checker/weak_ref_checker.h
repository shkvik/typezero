#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class WeakRefChecker {
public:
  explicit WeakRefChecker(Checker &checker);

  const Type *resolveWeakRefType(const TypeRef &ref);
  bool checkWeakTarget(const Type *target, llvm::SMLoc loc);
  Expr *checkWeakRefNew(NewExpr *expr, const Type *expected);
  Expr *checkWeakRefMethod(CallExpr *call, MemberExpr *member);

private:
  Checker &checker_;
};

}
