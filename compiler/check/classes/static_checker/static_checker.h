#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Static members and static calls.
class StaticChecker {
public:
  explicit StaticChecker(Checker &checker);

  // `C.x` and `C.m`: static fields are globals, static methods functions.
  Expr *checkStaticMember(MemberExpr *member, ClassDecl *cls, const Type *expected);
  // An assignment to `C.x`: the global it is.
  const Type *checkStaticTarget(Expr *&target, ClassDecl *cls);
  Expr *checkClassStaticCall(CallExpr *call, MemberExpr *member, ClassDecl *cls,
                             const Type *expected = nullptr);

private:
  Checker &checker_;
};

} // namespace tz
