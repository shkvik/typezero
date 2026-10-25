#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Calls of methods and new of a class.
class MethodCallChecker {
public:
  explicit MethodCallChecker(Sema &sema);

  Expr *checkMethodCall(CallExpr *call, MemberExpr *member, const Type *expected = nullptr);
  Expr *checkClassNew(NewExpr *expr, ClassDecl *cls, const Type *expected = nullptr);

private:
  Sema &sema_;
};

} // namespace tz
