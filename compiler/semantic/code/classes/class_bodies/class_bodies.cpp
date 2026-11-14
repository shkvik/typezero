#include "compiler/semantic/code/classes/class_bodies/class_bodies.h"

namespace tz {

ClassBodies::ClassBodies(SemanticState &owner, ClassFacts &sharedFacts)
    : classBodyWalks(owner, sharedFacts),
      lateClassWalks(owner, sharedFacts),
      classStatements(owner, sharedFacts) {}

void ClassBodies::checkClassStmt(const CheckClassStmt &task) {
  return this->classStatements.checkClassStmt(task);
}

void ClassBodies::checkClassExpr(const CheckClassExpr &task) {
  return this->classStatements.checkClassExpr(task);
}

void ClassBodies::afterLateClassExpr(const AfterLateClassExpr &task) {
  return this->classStatements.afterLateClassExpr(task);
}

void ClassBodies::finishClassExpr(const FinishClassExpr &task) {
  return this->classStatements.finishClassExpr(task);
}

void ClassBodies::walkStaticFields(const WalkStaticFields &task) {
  return this->classStatements.walkStaticFields(task);
}

void ClassBodies::leaveStaticFields() {
  return this->classStatements.leaveStaticFields();
}

void ClassBodies::walkStaticField(const WalkStaticField &task) {
  return this->classStatements.walkStaticField(task);
}

void ClassBodies::afterStaticField(const AfterStaticField &task) {
  return this->classStatements.afterStaticField(task);
}

void ClassBodies::afterLateBase(const AfterLateBase &task) {
  return this->classStatements.afterLateBase(task);
}

void ClassBodies::leaveDeclaringClass() {
  return this->classStatements.leaveDeclaringClass();
}

void ClassBodies::finishLateClass(const FinishLateClass &task) {
  return this->classStatements.finishLateClass(task);
}

void ClassBodies::walkClassBodies(const WalkClassBodies &task) {
  return this->classBodyWalks.walkClassBodies(task);
}

void ClassBodies::walkClassFunction(const WalkClassFunction &task) {
  return this->classBodyWalks.walkClassFunction(task);
}

void ClassBodies::walkFieldStarts(const WalkFieldStarts &task) {
  return this->classBodyWalks.walkFieldStarts(task);
}

void ClassBodies::walkClassMethod(const WalkClassMethod &task) {
  return this->classBodyWalks.walkClassMethod(task);
}

void ClassBodies::afterClassMethodReturn(const AfterClassMethodReturn &task) {
  return this->classBodyWalks.afterClassMethodReturn(task);
}

void ClassBodies::finishClassBodies(const FinishClassBodies &task) {
  return this->classBodyWalks.finishClassBodies(task);
}

void ClassBodies::pushLateClassTasks() {
  return this->lateClassWalks.pushLateClassTasks();
}

void ClassBodies::nextLateClass() {
  return this->lateClassWalks.nextLateClass();
}

void ClassBodies::walkLateClass(const WalkLateClass &task) {
  return this->lateClassWalks.walkLateClass(task);
}

void ClassBodies::finishLateClassWalk(const FinishLateClassWalk &task) {
  return this->lateClassWalks.finishLateClassWalk(task);
}

void ClassBodies::bindOuterTypeParams(const BindOuterTypeParams &task) {
  return this->classBodyWalks.bindOuterTypeParams(task);
}

void ClassBodies::declareThis(const DeclareThis &task) {
  return this->classBodyWalks.declareThis(task);
}

void ClassBodies::checkFieldsSet(const CheckFieldsSet &task) {
  return this->classBodyWalks.checkFieldsSet(task);
}

void ClassBodies::resolveConstructorType(const ResolveConstructorType &task) {
  return this->classStatements.resolveConstructorType(task);
}

void ClassBodies::afterConstructorTypeArgument(const AfterConstructorTypeArgument &task) {
  return this->classStatements.afterConstructorTypeArgument(task);
}

void ClassBodies::checkConstructorArgs(const CheckConstructorArgs &task) {
  return this->classStatements.checkConstructorArgs(task);
}

void ClassBodies::rememberEmittedLateClasses() {
  return this->lateClassWalks.rememberEmittedLateClasses();
}

void ClassBodies::markLateOverrides(SourceFile &program) {
  return this->lateClassWalks.markLateOverrides(program);
}

void ClassBodies::walkConstructorCalls() {
  return this->lateClassWalks.walkConstructorCalls();
}

}
