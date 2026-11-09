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
  using ResultTaskCallbackOk = Result<const Type *, TimerCheckerTaskCallbackError>;

  bool rearms(const Expr *callback) const;
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
