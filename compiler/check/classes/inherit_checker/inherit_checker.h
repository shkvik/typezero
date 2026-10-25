#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// super, abstract and implements.
class InheritChecker {
public:
  explicit InheritChecker(Checker &checker);

  // What `cls` must implement: the abstract methods up its chain, and the
  // members of the interfaces it implements.
  void checkAbstract(ClassDecl *cls);
  void checkImplements(ClassDecl *cls);
  // `super(args)` in a constructor and `super.m(args)` in a method.
  Expr *checkSuperCall(CallExpr *call);
  Expr *checkSuperMethodCall(CallExpr *call, MemberExpr *member);

private:
  Checker &checker_;
};

} // namespace tz
