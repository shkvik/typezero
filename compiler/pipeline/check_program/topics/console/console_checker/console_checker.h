#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class ConsoleChecker {
public:
  explicit ConsoleChecker(CheckState &owner);

  Expr *checkConsoleLog(CallExpr *call);
  Expr *checkConsoleCall(CallExpr *call, MemberExpr *member);

private:
  CheckState &state;
};

}
