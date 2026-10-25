#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Calls of methods and new of a class.
class MethodCallChecker {
public:
  explicit MethodCallChecker(Checker &checker);

  Expr *checkMethodCall(CallExpr *call, MemberExpr *member, const Type *expected = nullptr);
  Expr *checkClassNew(NewExpr *expr, ClassDecl *cls, const Type *expected = nullptr);

private:
  Checker &checker_;
};

} // namespace tz
