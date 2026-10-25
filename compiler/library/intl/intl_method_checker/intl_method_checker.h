#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The methods of Intl objects, toLocaleString and localeCompare.
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

} // namespace tz
