#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class TimerChecker {
public:
  explicit TimerChecker(Checker &checker);

  Expr *checkTimersPromisesCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc,
                                const Type *expected = nullptr);
  Expr *checkSchedulerCall(CallExpr *call, MemberExpr *member);
  Expr *checkTaskCallback(Expr *arg, llvm::StringRef fn);
  Expr *checkAsyncGlobal(CallExpr *call, NameExpr *name);
  Expr *checkNextTick(CallExpr *call, MemberExpr *member);
  const Type *timerCallType(const Expr *init) const;
  Expr *checkTimerMethod(CallExpr *call, MemberExpr *member);

private:
  Checker &checker_;
};

}
