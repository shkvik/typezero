#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class IntlMethodChecker {
public:
  explicit IntlMethodChecker(Checker &checker);

  Expr *checkIntlMethod(CallExpr *call, MemberExpr *member);
  Expr *checkNumberLocale(CallExpr *call, MemberExpr *member);
  Expr *checkStringLocale(CallExpr *call, MemberExpr *member);
  Expr *checkIntlNumber(Expr *arg, llvm::StringRef method);

private:
  Checker &checker_;
};

}
