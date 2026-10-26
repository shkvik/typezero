#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class StaticChecker {
public:
  explicit StaticChecker(Checker &checker);

  Expr *checkStaticMember(MemberExpr *member, ClassDecl *cls, const Type *expected);
  const Type *checkStaticTarget(Expr *&target, ClassDecl *cls);
  Expr *checkClassStaticCall(CallExpr *call, MemberExpr *member, ClassDecl *cls,
                             const Type *expected = nullptr);

private:
  Checker &checker_;
};

}
