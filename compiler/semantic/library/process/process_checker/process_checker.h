#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/process/process_checker/process_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class CallChecker;
class DateChecker;
class ExprChecker;
class TimerChecker;
class ResultChecker;
class StreamSources;
class TypeRelations;
class ArgumentChecker;
class EventTargetChecker;

class ProcessChecker {
public:
  ProcessChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkProcessCall(CallExpr *call, MemberExpr *member);
  Expr *checkProcessMember(MemberExpr *member);
  Expr *checkProcessEnvIndex(IndexExpr *index);
  Expr *checkProcessAssign(AssignExpr *assign);
  bool isProcessMember(const Expr *expr, llvm::StringRef name) const;
  Expr *checkExitCode(Expr *code);
  Expr *builtinCall(llvm::SMLoc loc, Expr *callee, Builtin builtin, std::vector<Expr *> args,
                    const Type *type);

private:
  using ResultListenedSignal = Result<llvm::StringRef, ProcessCheckerListenedSignalError>;

  static ResultListenedSignal listenedSignal(llvm::StringRef name, const std::u16string &event);
  static std::u16string toUtf16(llvm::StringRef text);

  Expr *checkSignalCall(CallExpr *call, MemberExpr *member);

  SemanticState &state;
  CallChecker *callChecker = nullptr;
  DateChecker *dateChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  TimerChecker *timerChecker = nullptr;
  ResultChecker *resultChecker = nullptr;
  StreamSources *streamSources = nullptr;
  TypeRelations *typeRelations = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  EventTargetChecker *eventTargetChecker = nullptr;
};

}
