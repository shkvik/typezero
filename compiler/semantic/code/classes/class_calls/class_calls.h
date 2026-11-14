#pragma once

#include "compiler/semantic/code/classes/class_calls/class_call_rules/class_call_rules.h"
#include "compiler/semantic/code/classes/class_calls/method_calls/method_calls.h"
#include "compiler/semantic/code/classes/class_calls/new_expressions/new_expressions.h"
#include "compiler/semantic/code/classes/class_calls/super_calls/super_calls.h"
#include "compiler/semantic/code/classes/class_facts/class_facts.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class ClassCalls {
public:
  ClassCalls(SemanticState &owner, ClassFacts &sharedFacts);

  void setCallType(const SetCallType &task);
  void checkClassNew(const CheckClassNew &task);
  void checkValueNew(const CheckValueNew &task);
  void checkSuperCall(const CheckSuperCall &task);
  void finishClassNew(const FinishClassNew &task);
  void afterBoundTypes(const AfterBoundTypes &task);
  void checkMethodCall(const CheckMethodCall &task);
  void finishSuperCall(const FinishSuperCall &task);
  void afterSuperObject(const AfterSuperObject &task);
  void nextReflectedArg(const NextReflectedArg &task);
  void afterMethodChoice(const AfterMethodChoice &task);
  void afterReflectedArg(const AfterReflectedArg &task);
  void checkOverloadCall(const CheckOverloadCall &task);
  void afterGenericCallee(const AfterGenericCallee &task);
  void afterMethodCallTry(const AfterMethodCallTry &task);
  void afterOverloadChoice(const AfterOverloadChoice &task);
  void afterValueNewCallee(const AfterValueNewCallee &task);
  void afterPlainMethodType(const AfterPlainMethodType &task);
  void afterSuperEmitterTry(const AfterSuperEmitterTry &task);
  void checkClassStaticCall(const CheckClassStaticCall &task);
  void checkSuperMethodCall(const CheckSuperMethodCall &task);
  void afterClassFieldCallee(const AfterClassFieldCallee &task);
  void startGenericCallTypes(const StartGenericCallTypes &task);
  void afterConstructorChoice(const AfterConstructorChoice &task);
  void afterEventsFunctionTry(const AfterEventsFunctionTry &task);
  void afterGenericMethodCall(const AfterGenericMethodCall &task);
  void afterStaticFieldCallee(const AfterStaticFieldCallee &task);
  void afterSuperMethodChoice(const AfterSuperMethodChoice &task);
  void checkConstructorAccess(const CheckConstructorAccess &task);
  void afterStaticMethodChoice(const AfterStaticMethodChoice &task);
  void afterParentConstructorChoice(const AfterParentConstructorChoice &task);

private:
  ClassCallRules classCallRules;
  SuperCalls superCalls;
  MethodCalls methodCalls;
  NewExpressions newExpressions;
};

}
