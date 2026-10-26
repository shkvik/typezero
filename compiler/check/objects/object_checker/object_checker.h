#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ObjectChecker {
public:
  explicit ObjectChecker(Checker &checker);

  Expr *checkObjectCall(CallExpr *call, MemberExpr *member, const Type *expected);
  void checkForIn(ForOfStmt *stmt);
  Expr *checkField(MemberExpr *member);
  Expr *checkFieldCall(CallExpr *call, MemberExpr *member);
  const Type *checkObjectFieldTarget(MemberExpr *member);
  std::string objectMismatch(const Type *from, const Type *to, bool conflict = false) const;

private:
  Checker &checker_;
};

}
