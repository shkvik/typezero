#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ResultChecker {
public:
  explicit ResultChecker(Checker &checker);

  Expr *checkResultCall(CallExpr *call, NameExpr *name, const Type *expected);
  Expr *checkResultStatic(CallExpr *call, MemberExpr *member);
  Expr *checkResultMember(MemberExpr *member);
  Expr *checkOkErr(CallExpr *call, NameExpr *name, bool isOk, const Type *expected);
  void checkDiscarded(const Expr *expr, bool callback = false, llvm::SMLoc at = {});
  void addResultFacts(const Expr *cond, Checker::Flow &whenTrue, Checker::Flow &whenFalse) const;
  Expr *checkFromThrowable(CallExpr *call, llvm::SMLoc loc);

private:
  Checker &checker_;
};

}
