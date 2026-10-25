#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The methods of Intl objects, toLocaleString and localeCompare.
class IntlMethodChecker {
public:
  explicit IntlMethodChecker(Sema &sema);

  Expr *checkIntlMethod(CallExpr *call, MemberExpr *member);
  Expr *checkNumberLocale(CallExpr *call, MemberExpr *member);
  Expr *checkStringLocale(CallExpr *call, MemberExpr *member);
  Expr *checkIntlNumber(Expr *arg, llvm::StringRef method);

private:
  Sema &sema_;
};

} // namespace tz
