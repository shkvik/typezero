#pragma once

#include "compiler/semantic/code/calls/call_arguments/call_arguments.h"
#include "compiler/semantic/code/calls/call_dispatch/call_dispatch.h"
#include "compiler/semantic/code/calls/call_rules/call_rules.h"
#include "compiler/semantic/code/calls/named_calls/named_calls.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class CodeCallsRegistry {
public:
  explicit CodeCallsRegistry(SemanticState &owner);

  void checkNew(const CheckNew &task);
  void checkCall(const CheckCall &task);
  void scopedCall(const ScopedCall &task);
  void checkArgSlot(const CheckArgSlot &task);
  void afterNewLoad(const AfterNewLoad &task);
  void afterClassNew(const AfterClassNew &task);
  void afterSubtleTry(const AfterSubtleTry &task);
  void checkValueCall(const CheckValueCall &task);
  void checkArguments(const CheckArguments &task);
  void afterConsoleTry(const AfterConsoleTry &task);
  void afterProcessTry(const AfterProcessTry &task);
  void checkDirectCall(const CheckDirectCall &task);
  void afterSpreadRest(const AfterSpreadRest &task);
  void walkArgsLoosely(const WalkArgsLoosely &task);
  void afterNodeCallTry(const AfterNodeCallTry &task);
  void afterCalleeValue(const AfterCalleeValue &task);
  void afterArrayLength(const AfterArrayLength &task);
  void afterNamedCallTry(const AfterNamedCallTry &task);
  void afterArrayElement(const AfterArrayElement &task);
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
  void afterDirectReturnType(const AfterDirectReturnType &task);
  void afterKnownFunctionType(const AfterKnownFunctionType &task);

private:
  CallRules callRules;
  NamedCalls namedCalls;
  CallDispatch callDispatch;
  CallArguments callArguments;
};

}
