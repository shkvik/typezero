#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/console/console_checker/console_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class ConsoleChecker {
public:
  explicit ConsoleChecker(CheckState &owner);

  Expr *checkConsoleLog(CallExpr *call);
  Expr *checkConsoleCall(CallExpr *call, MemberExpr *member);

private:
  using ResultPrintableItem = Result<const Expr *, ConsoleCheckerPrintableError>;
  using ResultSpreadableArg = Result<const Expr *, ConsoleCheckerSpreadableError>;
  using ResultConsoleMethod = Result<const MemberExpr *, ConsoleCheckerConsoleMethodError>;

  static ResultPrintableItem printable(const Expr *arg);
  static ResultConsoleMethod consoleMethod(const MemberExpr *member);
  static ResultSpreadableArg spreadable(const CallExpr *call, size_t i);

  CheckState &state;
};

}
