#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// console.log, info, debug, error and warn.
class ConsoleChecker {
public:
  explicit ConsoleChecker(Sema &sema);

  Expr *checkConsoleLog(CallExpr *call);
  // `console.x(...)`; null when the object is not the global console.
  Expr *checkConsoleCall(CallExpr *call, MemberExpr *member);

private:
  Sema &sema_;
};

} // namespace tz
