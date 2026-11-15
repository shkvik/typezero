#pragma once

#include "compiler/program/library/process/process_function_table/process_function_table.h"
#include "compiler/program/library/process/process_member_table/process_member_table.h"
#include "compiler/program/library/process/std_stream_member_table/std_stream_member_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library_v2/process/library_process_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>
#include <string>

namespace tz {

class LibraryProcessRegistry {
public:
  explicit LibraryProcessRegistry(SemanticState &owner);

  void exitCodeSlot(const ExitCodeSlot &task);
  void afterExitCode(const AfterExitCode &task);
  void afterEnvIndexKey(const AfterEnvIndexKey &task);
  void checkProcessCall(const CheckProcessCall &task);
  void checkProcessAssign(const CheckProcessAssign &task);
  void checkProcessMember(const CheckProcessMember &task);
  void afterProcessStream(const AfterProcessStream &task);
  void afterStdStreamChunk(const AfterStdStreamChunk &task);
  void afterSignalListener(const AfterSignalListener &task);
  void checkProcessEnvIndex(const CheckProcessEnvIndex &task);
  void afterAssignedExitCode(const AfterAssignedExitCode &task);

private:
  using ResultListenedSignal = Result<llvm::StringRef, LibraryProcessRegistryListenedSignalError>;

  void failAssign(AssignExpr *assign);
  void checkExit(CallExpr *call, const MemberExpr *member);
  void checkFieldAssign(AssignExpr *assign, const MemberExpr *member);
  void rejectStreamAssign(AssignExpr *assign, const MemberExpr *stream);
  void rejectPerformanceCall(CallExpr *call, const MemberExpr *member);
  void checkSignalCall(CallExpr *call, MemberExpr *member, ProcessMember method);
  void checkProcessValue(MemberExpr *member, std::optional<ProcessMember> found);
  void checkStdStreamCall(CallExpr *call, const MemberExpr *member, ProcessMember stream);
  void checkProcessListenerCall(CallExpr *call, MemberExpr *member,
                                std::optional<ProcessMember> found);
  void checkClockNow(CallExpr *call, const MemberExpr *member, Builtin builtin, const Type *result);
  void checkNoArgCall(CallExpr *call, const MemberExpr *member, Builtin builtin,
                      const Type *result);


  bool hasStreamLibrary() const;
  bool hasSignalLibrary() const;
  bool isThrownEvent(const CallExpr *call) const;
  bool isStream(std::optional<ProcessMember> found) const;
  bool isProperty(std::optional<ProcessMember> found) const;
  bool isSubscribing(std::optional<ProcessMember> found) const;
  bool isSignalMethod(std::optional<ProcessMember> found) const;


  Expr *envValue(MemberExpr *member);
  Expr *targetObject(const Expr *target) const;
  Expr *rejectPerformanceMember(MemberExpr *member);
  Expr *processValue(MemberExpr *member, ProcessMember found);
  Expr *checkStdStreamMember(MemberExpr *member, ProcessMember stream);


  std::optional<ProcessMember> outputStream(const Expr *expr) const;
  ProcessFunction signalFunction(ProcessMember method, const Type *listener) const;
  ResultListenedSignal checkListenedSignal(llvm::StringRef name, const Expr *event) const;
  std::string processMemberMessage(llvm::StringRef name, std::optional<ProcessMember> found) const;

  SemanticState &state;
};

}
