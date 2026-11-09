#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/console/console_checker/console_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class CallChecker;
class ExprChecker;

class ConsoleChecker {
public:
  ConsoleChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkConsoleLog(CallExpr *call);
  Expr *checkConsoleCall(CallExpr *call, MemberExpr *member);

private:
  using ResultPrintableItem = Result<const Expr *, ConsoleCheckerPrintableError>;
  using ResultSpreadableArg = Result<const Expr *, ConsoleCheckerSpreadableError>;
  using ResultConsoleMethod = Result<const MemberExpr *, ConsoleCheckerConsoleMethodError>;

  static ResultPrintableItem printable(const Expr *arg);
  static ResultConsoleMethod consoleMethod(const MemberExpr *member);
  static ResultSpreadableArg spreadable(const CallExpr *call, size_t i);

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
};

}
