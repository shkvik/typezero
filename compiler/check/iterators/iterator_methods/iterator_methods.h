#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class IteratorMethods {
public:
  explicit IteratorMethods(Checker &checker);

  void markFused(Expr *expr) const;
  Expr *checkIteratorMethod(CallExpr *call, MemberExpr *member);
  Expr *checkArrayIterator(CallExpr *call, MemberExpr *member);
  Expr *checkIteratorFrom(CallExpr *call, MemberExpr *member);
  Expr *checkArrayFrom(CallExpr *call, MemberExpr *member);

private:
  Checker &checker_;
};

}
