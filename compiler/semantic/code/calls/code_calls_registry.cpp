#include "compiler/semantic/code/calls/code_calls_registry.h"

namespace tz {

CodeCallsRegistry::CodeCallsRegistry(SemanticState &owner)
    : callRules(owner),
      namedCalls(owner, this->callRules),
      callDispatch(owner, this->callRules),
      callArguments(owner, this->callRules) {}

void CodeCallsRegistry::checkCall(const CheckCall &task) {
  return this->callDispatch.checkCall(task);
}

void CodeCallsRegistry::afterNodeCallTry(const AfterNodeCallTry &task) {
  return this->callDispatch.afterNodeCallTry(task);
}

void CodeCallsRegistry::afterResultStaticTry(const AfterResultStaticTry &task) {
  return this->callDispatch.afterResultStaticTry(task);
}

void CodeCallsRegistry::afterConsoleTry(const AfterConsoleTry &task) {
  return this->callDispatch.afterConsoleTry(task);
}

void CodeCallsRegistry::afterProcessTry(const AfterProcessTry &task) {
  return this->callDispatch.afterProcessTry(task);
}

void CodeCallsRegistry::afterBytesStaticTry(const AfterBytesStaticTry &task) {
  return this->callDispatch.afterBytesStaticTry(task);
}

void CodeCallsRegistry::afterReceiverObject(const AfterReceiverObject &task) {
  return this->callDispatch.afterReceiverObject(task);
}

void CodeCallsRegistry::afterResultMethodTry(const AfterResultMethodTry &task) {
  return this->callDispatch.afterResultMethodTry(task);
}

void CodeCallsRegistry::afterDateMethodTry(const AfterDateMethodTry &task) {
  return this->callDispatch.afterDateMethodTry(task);
}

void CodeCallsRegistry::afterMatchReceiver(const AfterMatchReceiver &task) {
  return this->callDispatch.afterMatchReceiver(task);
}

void CodeCallsRegistry::afterCommonReceiver(const AfterCommonReceiver &task) {
  return this->callDispatch.afterCommonReceiver(task);
}

void CodeCallsRegistry::afterSubtleTry(const AfterSubtleTry &task) {
  return this->callDispatch.afterSubtleTry(task);
}

void CodeCallsRegistry::afterClassMethodCall(const AfterClassMethodCall &task) {
  return this->callDispatch.afterClassMethodCall(task);
}

void CodeCallsRegistry::afterNamedCallTry(const AfterNamedCallTry &task) {
  return this->callDispatch.afterNamedCallTry(task);
}

void CodeCallsRegistry::scopedCall(const ScopedCall &task) {
  return this->namedCalls.scopedCall(task);
}

void CodeCallsRegistry::afterKnownFunctionType(const AfterKnownFunctionType &task) {
  return this->namedCalls.afterKnownFunctionType(task);
}

void CodeCallsRegistry::afterFunctionNameTry(const AfterFunctionNameTry &task) {
  return this->callDispatch.afterFunctionNameTry(task);
}

void CodeCallsRegistry::afterNotCallableName(const AfterNotCallableName &task) {
  return this->callDispatch.afterNotCallableName(task);
}

void CodeCallsRegistry::afterCalleeValue(const AfterCalleeValue &task) {
  return this->callArguments.afterCalleeValue(task);
}

void CodeCallsRegistry::checkValueCall(const CheckValueCall &task) {
  return this->callArguments.checkValueCall(task);
}

void CodeCallsRegistry::checkDirectCall(const CheckDirectCall &task) {
  return this->namedCalls.checkDirectCall(task);
}

void CodeCallsRegistry::afterDirectReturnType(const AfterDirectReturnType &task) {
  return this->callArguments.afterDirectReturnType(task);
}

void CodeCallsRegistry::checkArguments(const CheckArguments &task) {
  return this->callArguments.checkArguments(task);
}

void CodeCallsRegistry::afterSpreadRest(const AfterSpreadRest &task) {
  return this->callArguments.afterSpreadRest(task);
}

void CodeCallsRegistry::walkArgsLoosely(const WalkArgsLoosely &task) {
  return this->callArguments.walkArgsLoosely(task);
}

void CodeCallsRegistry::checkArgSlot(const CheckArgSlot &task) {
  return this->callArguments.checkArgSlot(task);
}

void CodeCallsRegistry::checkNew(const CheckNew &task) {
  return this->namedCalls.checkNew(task);
}

void CodeCallsRegistry::afterNewLoad(const AfterNewLoad &task) {
  return this->namedCalls.afterNewLoad(task);
}

void CodeCallsRegistry::afterClassNew(const AfterClassNew &task) {
  return this->namedCalls.afterClassNew(task);
}

void CodeCallsRegistry::afterArrayElement(const AfterArrayElement &task) {
  return this->namedCalls.afterArrayElement(task);
}

void CodeCallsRegistry::afterArrayLength(const AfterArrayLength &task) {
  return this->namedCalls.afterArrayLength(task);
}

}
