#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Static members and static calls.
class StaticChecker {
public:
  explicit StaticChecker(Sema &sema);

  // `C.x` and `C.m`: static fields are globals, static methods functions.
  Expr *checkStaticMember(MemberExpr *member, ClassDecl *cls, const Type *expected);
  // An assignment to `C.x`: the global it is.
  const Type *checkStaticTarget(Expr *&target, ClassDecl *cls);
  Expr *checkClassStaticCall(CallExpr *call, MemberExpr *member, ClassDecl *cls,
                             const Type *expected = nullptr);

private:
  Sema &sema_;
};

} // namespace tz
