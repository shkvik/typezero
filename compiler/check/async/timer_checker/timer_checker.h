#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// setTimeout, setInterval, queueMicrotask, nextTick, node:timers/promises and the
// methods of timers.
class TimerChecker {
public:
  explicit TimerChecker(Checker &checker);

  Expr *checkTimersPromisesCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc,
                                const Type *expected = nullptr);
  // `scheduler.wait(ms)` and `scheduler.yield()`; the object of `member` is
  // the name of scheduler.
  Expr *checkSchedulerCall(CallExpr *call, MemberExpr *member);
  // The callback of a timer or a queue: `() => void`.
  Expr *checkTaskCallback(Expr *arg, llvm::StringRef fn);
  // setTimeout and the other globals of timers and queueMicrotask; null if
  // `name` is none of them.
  Expr *checkAsyncGlobal(CallExpr *call, NameExpr *name);
  Expr *checkNextTick(CallExpr *call, MemberExpr *member);
  // The type of what `init`, not checked yet, gives if it calls
  // setTimeout, setInterval or setImmediate; null otherwise.
  const Type *timerCallType(const Expr *init) const;
  Expr *checkTimerMethod(CallExpr *call, MemberExpr *member);

private:
  Checker &checker_;
};

} // namespace tz
