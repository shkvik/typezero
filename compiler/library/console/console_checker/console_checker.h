#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ConsoleChecker {
public:
  explicit ConsoleChecker(Checker &owner);

  Expr *checkConsoleLog(CallExpr *call);
  Expr *checkConsoleCall(CallExpr *call, MemberExpr *member);

private:
  Checker &checker;
};

}
