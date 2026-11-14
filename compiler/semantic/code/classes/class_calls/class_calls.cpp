#include "compiler/semantic/code/classes/class_calls/class_calls.h"

namespace tz {

ClassCalls::ClassCalls(SemanticState &owner, ClassFacts &sharedFacts)
    : classCallRules(owner),
      superCalls(owner, sharedFacts, this->classCallRules),
      methodCalls(owner, sharedFacts, this->classCallRules),
      newExpressions(owner, sharedFacts, this->classCallRules) {}

void ClassCalls::checkMethodCall(const CheckMethodCall &task) {
  return this->methodCalls.checkMethodCall(task);
}

void ClassCalls::afterMethodCallTry(const AfterMethodCallTry &task) {
  return this->methodCalls.afterMethodCallTry(task);
}

void ClassCalls::afterClassFieldCallee(const AfterClassFieldCallee &task) {
  return this->methodCalls.afterClassFieldCallee(task);
}

void ClassCalls::afterMethodChoice(const AfterMethodChoice &task) {
  return this->methodCalls.afterMethodChoice(task);
}

void ClassCalls::afterPlainMethodType(const AfterPlainMethodType &task) {
  return this->methodCalls.afterPlainMethodType(task);
}

void ClassCalls::afterGenericMethodCall(const AfterGenericMethodCall &task) {
  return this->methodCalls.afterGenericMethodCall(task);
}

void ClassCalls::afterBoundTypes(const AfterBoundTypes &task) {
  return this->methodCalls.afterBoundTypes(task);
}

void ClassCalls::afterGenericCallee(const AfterGenericCallee &task) {
  return this->methodCalls.afterGenericCallee(task);
}

void ClassCalls::checkSuperCall(const CheckSuperCall &task) {
  return this->superCalls.checkSuperCall(task);
}

void ClassCalls::afterParentConstructorChoice(const AfterParentConstructorChoice &task) {
  return this->superCalls.afterParentConstructorChoice(task);
}

void ClassCalls::finishSuperCall(const FinishSuperCall &task) {
  return this->superCalls.finishSuperCall(task);
}

void ClassCalls::checkSuperMethodCall(const CheckSuperMethodCall &task) {
  return this->superCalls.checkSuperMethodCall(task);
}

void ClassCalls::afterSuperObject(const AfterSuperObject &task) {
  return this->superCalls.afterSuperObject(task);
}

void ClassCalls::afterSuperEmitterTry(const AfterSuperEmitterTry &task) {
  return this->superCalls.afterSuperEmitterTry(task);
}

void ClassCalls::afterSuperMethodChoice(const AfterSuperMethodChoice &task) {
  return this->superCalls.afterSuperMethodChoice(task);
}

void ClassCalls::checkValueNew(const CheckValueNew &task) {
  return this->newExpressions.checkValueNew(task);
}

void ClassCalls::afterValueNewCallee(const AfterValueNewCallee &task) {
  return this->newExpressions.afterValueNewCallee(task);
}

void ClassCalls::nextReflectedArg(const NextReflectedArg &task) {
  return this->newExpressions.nextReflectedArg(task);
}

void ClassCalls::afterReflectedArg(const AfterReflectedArg &task) {
  return this->newExpressions.afterReflectedArg(task);
}

void ClassCalls::checkClassNew(const CheckClassNew &task) {
  return this->newExpressions.checkClassNew(task);
}

void ClassCalls::afterConstructorChoice(const AfterConstructorChoice &task) {
  return this->newExpressions.afterConstructorChoice(task);
}

void ClassCalls::finishClassNew(const FinishClassNew &task) {
  return this->newExpressions.finishClassNew(task);
}

void ClassCalls::checkConstructorAccess(const CheckConstructorAccess &task) {
  return this->newExpressions.checkConstructorAccess(task);
}

void ClassCalls::checkOverloadCall(const CheckOverloadCall &task) {
  return this->methodCalls.checkOverloadCall(task);
}

void ClassCalls::afterOverloadChoice(const AfterOverloadChoice &task) {
  return this->methodCalls.afterOverloadChoice(task);
}

void ClassCalls::checkClassStaticCall(const CheckClassStaticCall &task) {
  return this->methodCalls.checkClassStaticCall(task);
}

void ClassCalls::afterEventsFunctionTry(const AfterEventsFunctionTry &task) {
  return this->methodCalls.afterEventsFunctionTry(task);
}

void ClassCalls::afterStaticFieldCallee(const AfterStaticFieldCallee &task) {
  return this->methodCalls.afterStaticFieldCallee(task);
}

void ClassCalls::afterStaticMethodChoice(const AfterStaticMethodChoice &task) {
  return this->methodCalls.afterStaticMethodChoice(task);
}

}
