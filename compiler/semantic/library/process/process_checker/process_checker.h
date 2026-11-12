#pragma once

#include "compiler/program/library/process/process_function_table/process_function_table.h"
#include "compiler/program/library/process/process_member_table/process_member_table.h"
#include "compiler/program/library/process/std_stream_member_table/std_stream_member_table.h"
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

  Expr *checkExitCode(Expr *code);
  Expr *checkProcessMember(MemberExpr *member);
  Expr *checkProcessAssign(AssignExpr *assign);
  Expr *checkProcessEnvIndex(IndexExpr *index);
  Expr *checkProcessCall(CallExpr *call, MemberExpr *member);
  bool isProcessMember(const Expr *expr, ProcessMember which) const;
  Expr *builtinCall(llvm::SMLoc loc, Expr *callee, Builtin builtin, std::vector<Expr *> args,
                    const Type *type);

private:
  using ResultListenedSignal = Result<llvm::StringRef, ProcessCheckerListenedSignalError>;

  ResultListenedSignal listenedSignal(llvm::StringRef name, const Expr *event) const;

  bool hasStreamLibrary() const;
  bool hasSignalLibrary() const;
  Expr *writtenChunk(Expr *chunk);
  Expr *envValue(MemberExpr *member);
  void walkArgsLoosely(CallExpr *call);
  Expr *failedAssign(AssignExpr *assign);
  Expr *checkProcessField(MemberExpr *member);
  Expr *targetObject(const Expr *target) const;
  bool isThrownEvent(const CallExpr *call) const;
  Expr *rejectPerformanceMember(MemberExpr *member);
  bool isStream(std::optional<ProcessMember> found) const;
  bool isProperty(std::optional<ProcessMember> found) const;
  Expr *checkExit(CallExpr *call, const MemberExpr *member);
  Expr *processValue(MemberExpr *member, ProcessMember found);
  bool isSubscribing(std::optional<ProcessMember> found) const;
  bool isSignalMethod(std::optional<ProcessMember> found) const;
  Expr *checkNextTick(CallExpr *call, const MemberExpr *member);
  std::optional<ProcessMember> outputStream(const Expr *expr) const;
  Expr *checkStdStreamMember(MemberExpr *member, ProcessMember stream);
  Expr *checkFieldAssign(AssignExpr *assign, const MemberExpr *member);
  Expr *rejectStreamAssign(AssignExpr *assign, const MemberExpr *stream);
  Expr *rejectPerformanceCall(CallExpr *call, const MemberExpr *member);
  bool hasRejectedArgs(CallExpr *call, const MemberExpr *member, unsigned count);
  Expr *checkSignalCall(CallExpr *call, MemberExpr *member, ProcessMember method);
  ProcessFunction signalFunction(ProcessMember method, const Type *listener) const;
  Expr *checkStdStreamCall(CallExpr *call, const MemberExpr *member, ProcessMember stream);
  std::string processMemberMessage(llvm::StringRef name, std::optional<ProcessMember> found) const;
  Expr *checkProcessListenerCall(CallExpr *call, MemberExpr *member,
                                 std::optional<ProcessMember> found);
  Expr *checkClockNow(CallExpr *call, const MemberExpr *member, Builtin builtin,
                      const Type *result);
  Expr *checkNoArgCall(CallExpr *call, const MemberExpr *member, Builtin builtin,
                       const Type *result);

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
