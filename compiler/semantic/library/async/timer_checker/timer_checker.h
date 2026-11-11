#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/async/timer_checker/timer_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class FsChecker;
class CallChecker;
class ExprChecker;
class TypeRelations;
class ClosureChecker;
class EmitterChecker;
class ArgumentChecker;
class FunctionValueChecker;

class TimerChecker {
public:
  using ResultNextTickQueued = Result<Expr *, TimerCheckerCheckNextTickError>;
  using ResultTimerScheduled = Result<Expr *, TimerCheckerCheckAsyncGlobalError>;
  using ResultTimerMethodRun = Result<Expr *, TimerCheckerCheckTimerMethodError>;
  using ResultSchedulerAwait = Result<Expr *, TimerCheckerCheckSchedulerCallError>;
  using ResultPromiseOfTimer = Result<Expr *, TimerCheckerCheckTimersPromisesCallError>;

  TimerChecker(SemanticState &owner, SemanticParts &all);

  const Type *timerCallType(const Expr *init) const;
  ResultNextTickQueued checkNextTick(CallExpr *call);
  Expr *checkTaskCallback(Expr *arg, llvm::StringRef fn);
  ResultTimerScheduled checkAsyncGlobal(CallExpr *call, NameExpr *name);
  ResultTimerMethodRun checkTimerMethod(CallExpr *call, MemberExpr *member);
  ResultSchedulerAwait checkSchedulerCall(CallExpr *call, MemberExpr *member);
  ResultPromiseOfTimer checkTimersPromisesCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc,
                                               const Type *expected = nullptr);

private:
  struct TimerValue {
    const Type *value = nullptr;
    Expr *argument = nullptr;
  };

  using ResultTaskCallbackOk = Result<const Type *, TimerCheckerTaskCallbackError>;
  using ResultTimerValueKept = Result<TimerValue, TimerCheckerCheckTimersPromisesCallError>;

  bool rearms(const Expr *callback) const;
  bool isTimerName(const NameExpr *name) const;
  static const Type *promisedHint(const Type *expected);
  static Expr *promisedTimer(CallExpr *call, const Type *value, bool immediate, bool interval);
  int receiverBits(Expr *callback, bool interval);
  void appendReceiverArgs(CallExpr *call, int bits);
  Expr *checkRefOption(Expr *ref, llvm::SMLoc loc);
  Expr *checkDelayArg(CallExpr *call, llvm::SMLoc loc);
  ResultTimerScheduled checkClearCall(CallExpr *call, bool immediate);
  ResultTimerScheduled checkTimeoutCall(CallExpr *call, llvm::StringRef fn, bool interval);
  ResultTimerScheduled checkTaskQueueCall(CallExpr *call, llvm::StringRef fn, bool immediate);
  ResultTimerValueKept timerValue(CallExpr *call, size_t valueAt, llvm::StringRef fn,
                                  const Type *expected);
  static ResultTaskCallbackOk taskCallback(const Type *type, const Type *task, const Expr *adapted,
                                           llvm::StringRef fn);

  SemanticState &state;
  FsChecker *fsChecker = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ClosureChecker *closureChecker = nullptr;
  EmitterChecker *emitterChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  FunctionValueChecker *functionValueChecker = nullptr;
};

}
