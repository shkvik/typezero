#pragma once

#include "compiler/program/library/array/array_method_table/array_method_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/code/calls/call_rules/call_rules.h"
#include "compiler/semantic/code/calls/code_calls_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class CallDispatch {
public:
  CallDispatch(SemanticState &owner, CallRules &sharedCallRules);

  void checkCall(const CheckCall &task);
  void afterSubtleTry(const AfterSubtleTry &task);
  void afterConsoleTry(const AfterConsoleTry &task);
  void afterProcessTry(const AfterProcessTry &task);
  void afterNodeCallTry(const AfterNodeCallTry &task);
  void afterNamedCallTry(const AfterNamedCallTry &task);
  void afterDateMethodTry(const AfterDateMethodTry &task);
  void afterMatchReceiver(const AfterMatchReceiver &task);
  void afterBytesStaticTry(const AfterBytesStaticTry &task);
  void afterReceiverObject(const AfterReceiverObject &task);
  void afterCommonReceiver(const AfterCommonReceiver &task);
  void afterResultStaticTry(const AfterResultStaticTry &task);
  void afterResultMethodTry(const AfterResultMethodTry &task);
  void afterClassMethodCall(const AfterClassMethodCall &task);
  void afterFunctionNameTry(const AfterFunctionNameTry &task);
  void afterNotCallableName(const AfterNotCallableName &task);

private:
  using ResultTypeArgsAbsent = Result<const MemberExpr *, CodeCallsRegistryNoTypeArgsError>;

  void rejectTypeArgs(MemberExpr *member);
  void rewriteSymbolCallee(CallExpr *call);
  void rejectGlobalTypeArgs(MemberExpr *member);
  void nextNamedCall(const NamedCallDraft &draft);
  void reportMissingMethod(const MemberExpr *member, const Type *receiver);
  void startScopedCall(CallExpr *call, NameExpr *name, const Type *expected);
  void startMemberCall(CallExpr *call, MemberExpr *member, const Type *expected);
  void startOtherReceiver(CallExpr *call, MemberExpr *member, const Type *expected);
  void startReceiverObject(CallExpr *call, MemberExpr *member, const Type *expected);
  void startShapedReceiver(CallExpr *call, MemberExpr *member, const Type *expected);
  void startLibraryReceiver(CallExpr *call, MemberExpr *member, const Type *expected);
  void settledAfterSubtle(CallExpr *call, MemberExpr *member, const Type *receiver,
                          const Type *expected);
  void startSettledReceiver(CallExpr *call, MemberExpr *member, const Type *receiver,
                            const Type *expected);
  void startNonTupleReceiver(CallExpr *call, MemberExpr *member, const Type *receiver,
                             const Type *expected);
  void startPlainValueMethod(CallExpr *call, MemberExpr *member, const Type *receiver,
                             const Type *expected);


  Expr *arrayCopy(Expr *tuple);
  Expr *tupleArrayCopy(MemberExpr *member, const ArrayMethodInfo *arrayMethod);


  ResultTypeArgsAbsent checkNoTypeArgs(const MemberExpr *member) const;

  CallRules &callRules;
  SemanticState &state;
};

}
