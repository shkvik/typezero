#pragma once

#include "compiler/check/async/timer_checker/timer_checker_errors.h"
#include "compiler/check/checker/checker.h"
#include "compiler/result/result/result.h"

namespace tz {

class TimerChecker {
public:
  using ResultPromiseOfTimer = Result<Expr *, TimerCheckerCheckTimersPromisesCallError>;
  using ResultSchedulerAwait = Result<Expr *, TimerCheckerCheckSchedulerCallError>;
  using ResultTimerScheduled = Result<Expr *, TimerCheckerCheckAsyncGlobalError>;
  using ResultNextTickQueued = Result<Expr *, TimerCheckerCheckNextTickError>;
  using ResultTimerMethodRun = Result<Expr *, TimerCheckerCheckTimerMethodError>;

  explicit TimerChecker(Checker &owner);

  Expr *checkTaskCallback(Expr *arg, llvm::StringRef fn);
  const Type *timerCallType(const Expr *init) const;
  ResultTimerScheduled checkAsyncGlobal(CallExpr *call, NameExpr *name);
  ResultNextTickQueued checkNextTick(CallExpr *call, MemberExpr *member);
  ResultTimerMethodRun checkTimerMethod(CallExpr *call, MemberExpr *member);
  ResultSchedulerAwait checkSchedulerCall(CallExpr *call, MemberExpr *member);
  ResultPromiseOfTimer checkTimersPromisesCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc,
                                               const Type *expected = nullptr);

private:
  using ResultTaskCallbackOk = Result<const Type *, TimerCheckerTaskCallbackError>;

  static ResultTaskCallbackOk taskCallback(const Type *type, const Type *task, const Expr *adapted,
                                           llvm::StringRef fn);
  bool rearms(const Expr *callback) const;

  Checker &checker;
};

}
