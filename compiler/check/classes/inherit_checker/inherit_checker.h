#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class InheritChecker {
public:
  explicit InheritChecker(Checker &checker);

  void checkAbstract(ClassDecl *cls);
  void checkImplements(ClassDecl *cls);
  Expr *checkSuperCall(CallExpr *call);
  Expr *checkSuperMethodCall(CallExpr *call, MemberExpr *member);

private:
  Checker &checker_;
};

}
