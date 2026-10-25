#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// console.log, info, debug, error and warn.
class ConsoleChecker {
public:
  explicit ConsoleChecker(Checker &checker);

  Expr *checkConsoleLog(CallExpr *call);
  // `console.x(...)`; null when the object is not the global console.
  Expr *checkConsoleCall(CallExpr *call, MemberExpr *member);

private:
  Checker &checker_;
};

} // namespace tz
