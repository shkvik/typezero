#pragma once

#include "compiler/program/library/console/console_method_table/console_method_table.h"
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
  using ResultConsoleMethod = Result<ConsoleMethod, ConsoleCheckerConsoleMethodError>;

  ResultPrintableItem checkPrintable(const Expr *arg) const;
  ResultConsoleMethod checkConsoleMethod(const MemberExpr *member) const;
  ResultSpreadableArg checkSpreadable(const CallExpr *call, size_t index) const;

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
};

}
