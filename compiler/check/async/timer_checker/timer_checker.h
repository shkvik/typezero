#pragma once

#include "compiler/check/async/timer_checker/timer_checker_errors.h"
#include "compiler/check/checker/checker.h"
#include "compiler/result/result/result.h"

namespace tz {

class TimerChecker {
public:
  using ResultNextTickQueued = Result<Expr *, TimerCheckerCheckNextTickError>;
  using ResultTimerScheduled = Result<Expr *, TimerCheckerCheckAsyncGlobalError>;
  using ResultTimerMethodRun = Result<Expr *, TimerCheckerCheckTimerMethodError>;
  using ResultSchedulerAwait = Result<Expr *, TimerCheckerCheckSchedulerCallError>;
  using ResultPromiseOfTimer = Result<Expr *, TimerCheckerCheckTimersPromisesCallError>;

  explicit TimerChecker(Checker &owner);

  const Type *timerCallType(const Expr *init) const;
  ResultNextTickQueued checkNextTick(CallExpr *call);
  Expr *checkTaskCallback(Expr *arg, llvm::StringRef fn);
  ResultTimerScheduled checkAsyncGlobal(CallExpr *call, NameExpr *name);
  ResultTimerMethodRun checkTimerMethod(CallExpr *call, MemberExpr *member);
  ResultSchedulerAwait checkSchedulerCall(CallExpr *call, MemberExpr *member);
  ResultPromiseOfTimer checkTimersPromisesCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc,
                                               const Type *expected = nullptr);

private:
  using ResultTaskCallbackOk = Result<const Type *, TimerCheckerTaskCallbackError>;

  bool rearms(const Expr *callback) const;
  static ResultTaskCallbackOk taskCallback(const Type *type, const Type *task, const Expr *adapted,
                                           llvm::StringRef fn);

  Checker &checker;
};

}
