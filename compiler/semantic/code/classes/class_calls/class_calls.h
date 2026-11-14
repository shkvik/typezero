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

  void checkValueNew(const CheckValueNew &task);
  void checkClassNew(const CheckClassNew &task);
  void checkSuperCall(const CheckSuperCall &task);
  void finishClassNew(const FinishClassNew &task);
  void checkMethodCall(const CheckMethodCall &task);
  void afterBoundTypes(const AfterBoundTypes &task);
  void finishSuperCall(const FinishSuperCall &task);
  void afterSuperObject(const AfterSuperObject &task);
  void nextReflectedArg(const NextReflectedArg &task);
  void afterMethodChoice(const AfterMethodChoice &task);
  void afterReflectedArg(const AfterReflectedArg &task);
  void checkOverloadCall(const CheckOverloadCall &task);
  void afterMethodCallTry(const AfterMethodCallTry &task);
  void afterGenericCallee(const AfterGenericCallee &task);
  void afterValueNewCallee(const AfterValueNewCallee &task);
  void afterOverloadChoice(const AfterOverloadChoice &task);
  void afterPlainMethodType(const AfterPlainMethodType &task);
  void checkSuperMethodCall(const CheckSuperMethodCall &task);
  void afterSuperEmitterTry(const AfterSuperEmitterTry &task);
  void checkClassStaticCall(const CheckClassStaticCall &task);
  void afterClassFieldCallee(const AfterClassFieldCallee &task);
  void afterGenericMethodCall(const AfterGenericMethodCall &task);
  void afterSuperMethodChoice(const AfterSuperMethodChoice &task);
  void afterConstructorChoice(const AfterConstructorChoice &task);
  void afterEventsFunctionTry(const AfterEventsFunctionTry &task);
  void afterStaticFieldCallee(const AfterStaticFieldCallee &task);
  void afterStaticMethodChoice(const AfterStaticMethodChoice &task);
  void afterParentConstructorChoice(const AfterParentConstructorChoice &task);

private:
  ClassCallRules classCallRules;
  SuperCalls superCalls;
  MethodCalls methodCalls;
  NewExpressions newExpressions;
};

}
