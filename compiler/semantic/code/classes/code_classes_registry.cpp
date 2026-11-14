#include "compiler/semantic/code/classes/code_classes_registry.h"

namespace tz {

CodeClassesRegistry::CodeClassesRegistry(SemanticState &owner)
    : facts(owner),
      calls(owner, this->facts),
      bodies(owner, this->facts),
      members(owner, this->facts),
      overloads(owner, this->facts),
      decorators(owner, this->facts) {}

void CodeClassesRegistry::checkClassStmt(const CheckClassStmt &task) {
  return this->bodies.checkClassStmt(task);
}

void CodeClassesRegistry::checkClassExpr(const CheckClassExpr &task) {
  return this->bodies.checkClassExpr(task);
}

void CodeClassesRegistry::afterLateClassExpr(const AfterLateClassExpr &task) {
  return this->bodies.afterLateClassExpr(task);
}

void CodeClassesRegistry::finishClassExpr(const FinishClassExpr &task) {
  return this->bodies.finishClassExpr(task);
}

void CodeClassesRegistry::classValueType(const ClassValueType &task) {
  return this->members.classValueType(task);
}

void CodeClassesRegistry::walkStaticFields(const WalkStaticFields &task) {
  return this->bodies.walkStaticFields(task);
}

void CodeClassesRegistry::leaveStaticFields() {
  return this->bodies.leaveStaticFields();
}

void CodeClassesRegistry::walkStaticField(const WalkStaticField &task) {
  return this->bodies.walkStaticField(task);
}

void CodeClassesRegistry::afterStaticField(const AfterStaticField &task) {
  return this->bodies.afterStaticField(task);
}

void CodeClassesRegistry::decorateClass(const DecorateClass &task) {
  return this->decorators.decorateClass(task);
}

void CodeClassesRegistry::leaveClassContext() {
  return this->decorators.leaveClassContext();
}

void CodeClassesRegistry::afterLateBase(const AfterLateBase &task) {
  return this->bodies.afterLateBase(task);
}

void CodeClassesRegistry::leaveDeclaringClass() {
  return this->bodies.leaveDeclaringClass();
}

void CodeClassesRegistry::finishLateClass(const FinishLateClass &task) {
  return this->bodies.finishLateClass(task);
}

void CodeClassesRegistry::walkClassBodies(const WalkClassBodies &task) {
  return this->bodies.walkClassBodies(task);
}

void CodeClassesRegistry::walkClassFunction(const WalkClassFunction &task) {
  return this->bodies.walkClassFunction(task);
}

void CodeClassesRegistry::walkFieldStarts(const WalkFieldStarts &task) {
  return this->bodies.walkFieldStarts(task);
}

void CodeClassesRegistry::walkClassMethod(const WalkClassMethod &task) {
  return this->bodies.walkClassMethod(task);
}

void CodeClassesRegistry::afterClassMethodReturn(const AfterClassMethodReturn &task) {
  return this->bodies.afterClassMethodReturn(task);
}

void CodeClassesRegistry::finishClassBodies(const FinishClassBodies &task) {
  return this->bodies.finishClassBodies(task);
}

void CodeClassesRegistry::pushLateClassTasks() {
  return this->bodies.pushLateClassTasks();
}

void CodeClassesRegistry::nextLateClass() {
  return this->bodies.nextLateClass();
}

void CodeClassesRegistry::walkLateClass(const WalkLateClass &task) {
  return this->bodies.walkLateClass(task);
}

void CodeClassesRegistry::finishLateClassWalk(const FinishLateClassWalk &task) {
  return this->bodies.finishLateClassWalk(task);
}

void CodeClassesRegistry::bindOuterTypeParams(const BindOuterTypeParams &task) {
  return this->bodies.bindOuterTypeParams(task);
}

void CodeClassesRegistry::declareThis(const DeclareThis &task) {
  return this->bodies.declareThis(task);
}

void CodeClassesRegistry::checkFieldsSet(const CheckFieldsSet &task) {
  return this->bodies.checkFieldsSet(task);
}

void CodeClassesRegistry::makeMethodDescriptor(const MakeMethodDescriptor &task) {
  return this->decorators.makeMethodDescriptor(task);
}

void CodeClassesRegistry::resolveConstructorType(const ResolveConstructorType &task) {
  return this->bodies.resolveConstructorType(task);
}

void CodeClassesRegistry::afterConstructorTypeArgument(const AfterConstructorTypeArgument &task) {
  return this->bodies.afterConstructorTypeArgument(task);
}

void CodeClassesRegistry::checkConstructorArgs(const CheckConstructorArgs &task) {
  return this->bodies.checkConstructorArgs(task);
}

void CodeClassesRegistry::checkClassValue(const CheckClassValue &task) {
  return this->members.checkClassValue(task);
}

void CodeClassesRegistry::checkThis(const CheckThis &task) {
  return this->members.checkThis(task);
}

void CodeClassesRegistry::checkClassMember(const CheckClassMember &task) {
  return this->members.checkClassMember(task);
}

void CodeClassesRegistry::afterMethodVariants(const AfterMethodVariants &task) {
  return this->members.afterMethodVariants(task);
}

void CodeClassesRegistry::afterMemberFunctionType(const AfterMemberFunctionType &task) {
  return this->members.afterMemberFunctionType(task);
}

void CodeClassesRegistry::checkStaticMember(const CheckStaticMember &task) {
  return this->members.checkStaticMember(task);
}

void CodeClassesRegistry::afterStaticVariants(const AfterStaticVariants &task) {
  return this->members.afterStaticVariants(task);
}

void CodeClassesRegistry::afterNameFunctionType(const AfterNameFunctionType &task) {
  return this->members.afterNameFunctionType(task);
}

void CodeClassesRegistry::checkClassRefMember(const CheckClassRefMember &task) {
  return this->members.checkClassRefMember(task);
}

void CodeClassesRegistry::afterClassRefStatic(const AfterClassRefStatic &task) {
  return this->members.afterClassRefStatic(task);
}

void CodeClassesRegistry::checkOverloadMember(const CheckOverloadMember &task) {
  return this->members.checkOverloadMember(task);
}

void CodeClassesRegistry::afterOverloadVariants(const AfterOverloadVariants &task) {
  return this->members.afterOverloadVariants(task);
}

void CodeClassesRegistry::checkFieldTarget(const CheckFieldTarget &task) {
  return this->members.checkFieldTarget(task);
}

void CodeClassesRegistry::checkStaticTarget(const CheckStaticTarget &task) {
  return this->members.checkStaticTarget(task);
}

void CodeClassesRegistry::checkInstanceOfOperator(const CheckInstanceOfOperator &task) {
  return this->members.checkInstanceOfOperator(task);
}

void CodeClassesRegistry::afterInstanceOfLeft(const AfterInstanceOfLeft &task) {
  return this->members.afterInstanceOfLeft(task);
}

void CodeClassesRegistry::instanceOfRight(const InstanceOfRight &task) {
  return this->members.instanceOfRight(task);
}

void CodeClassesRegistry::afterInstanceOfRight(const AfterInstanceOfRight &task) {
  return this->members.afterInstanceOfRight(task);
}

void CodeClassesRegistry::checkMethodCall(const CheckMethodCall &task) {
  return this->calls.checkMethodCall(task);
}

void CodeClassesRegistry::afterMethodCallTry(const AfterMethodCallTry &task) {
  return this->calls.afterMethodCallTry(task);
}

void CodeClassesRegistry::afterClassFieldCallee(const AfterClassFieldCallee &task) {
  return this->calls.afterClassFieldCallee(task);
}

void CodeClassesRegistry::afterMethodChoice(const AfterMethodChoice &task) {
  return this->calls.afterMethodChoice(task);
}

void CodeClassesRegistry::afterPlainMethodType(const AfterPlainMethodType &task) {
  return this->calls.afterPlainMethodType(task);
}

void CodeClassesRegistry::afterGenericMethodCall(const AfterGenericMethodCall &task) {
  return this->calls.afterGenericMethodCall(task);
}

void CodeClassesRegistry::afterBoundTypes(const AfterBoundTypes &task) {
  return this->calls.afterBoundTypes(task);
}

void CodeClassesRegistry::afterGenericCallee(const AfterGenericCallee &task) {
  return this->calls.afterGenericCallee(task);
}

void CodeClassesRegistry::chooseVariant(const ChooseVariant &task) {
  return this->overloads.chooseVariant(task);
}

void CodeClassesRegistry::rankVariants(const RankVariants &task) {
  return this->overloads.rankVariants(task);
}

void CodeClassesRegistry::nextRankedVariant(const NextRankedVariant &task) {
  return this->overloads.nextRankedVariant(task);
}

void CodeClassesRegistry::afterGenericParams(const AfterGenericParams &task) {
  return this->overloads.afterGenericParams(task);
}

void CodeClassesRegistry::checkSuperCall(const CheckSuperCall &task) {
  return this->calls.checkSuperCall(task);
}

void CodeClassesRegistry::afterParentConstructorChoice(const AfterParentConstructorChoice &task) {
  return this->calls.afterParentConstructorChoice(task);
}

void CodeClassesRegistry::finishSuperCall(const FinishSuperCall &task) {
  return this->calls.finishSuperCall(task);
}

void CodeClassesRegistry::checkSuperMethodCall(const CheckSuperMethodCall &task) {
  return this->calls.checkSuperMethodCall(task);
}

void CodeClassesRegistry::afterSuperObject(const AfterSuperObject &task) {
  return this->calls.afterSuperObject(task);
}

void CodeClassesRegistry::afterSuperEmitterTry(const AfterSuperEmitterTry &task) {
  return this->calls.afterSuperEmitterTry(task);
}

void CodeClassesRegistry::afterSuperMethodChoice(const AfterSuperMethodChoice &task) {
  return this->calls.afterSuperMethodChoice(task);
}

void CodeClassesRegistry::checkValueNew(const CheckValueNew &task) {
  return this->calls.checkValueNew(task);
}

void CodeClassesRegistry::afterValueNewCallee(const AfterValueNewCallee &task) {
  return this->calls.afterValueNewCallee(task);
}

void CodeClassesRegistry::nextReflectedArg(const NextReflectedArg &task) {
  return this->calls.nextReflectedArg(task);
}

void CodeClassesRegistry::afterReflectedArg(const AfterReflectedArg &task) {
  return this->calls.afterReflectedArg(task);
}

void CodeClassesRegistry::checkClassNew(const CheckClassNew &task) {
  return this->calls.checkClassNew(task);
}

void CodeClassesRegistry::afterConstructorChoice(const AfterConstructorChoice &task) {
  return this->calls.afterConstructorChoice(task);
}

void CodeClassesRegistry::finishClassNew(const FinishClassNew &task) {
  return this->calls.finishClassNew(task);
}

void CodeClassesRegistry::checkOverloadCall(const CheckOverloadCall &task) {
  return this->calls.checkOverloadCall(task);
}

void CodeClassesRegistry::afterOverloadChoice(const AfterOverloadChoice &task) {
  return this->calls.afterOverloadChoice(task);
}

void CodeClassesRegistry::checkClassStaticCall(const CheckClassStaticCall &task) {
  return this->calls.checkClassStaticCall(task);
}

void CodeClassesRegistry::afterEventsFunctionTry(const AfterEventsFunctionTry &task) {
  return this->calls.afterEventsFunctionTry(task);
}

void CodeClassesRegistry::afterStaticFieldCallee(const AfterStaticFieldCallee &task) {
  return this->calls.afterStaticFieldCallee(task);
}

void CodeClassesRegistry::afterStaticMethodChoice(const AfterStaticMethodChoice &task) {
  return this->calls.afterStaticMethodChoice(task);
}

void CodeClassesRegistry::decorate(const Decorate &task) {
  return this->decorators.decorate(task);
}

void CodeClassesRegistry::nextHoistedDecorator(const NextHoistedDecorator &task) {
  return this->decorators.nextHoistedDecorator(task);
}

void CodeClassesRegistry::declareHoistedDecorator(const DeclareHoistedDecorator &task) {
  return this->decorators.declareHoistedDecorator(task);
}

void CodeClassesRegistry::afterDescriptorDeclared(const AfterDescriptorDeclared &task) {
  return this->decorators.afterDescriptorDeclared(task);
}

void CodeClassesRegistry::nextAppliedDecorator(const NextAppliedDecorator &task) {
  return this->decorators.nextAppliedDecorator(task);
}

void CodeClassesRegistry::afterDecoratorApplied(const AfterDecoratorApplied &task) {
  return this->decorators.afterDecoratorApplied(task);
}

void CodeClassesRegistry::relinkClass(const RelinkClass &task) {
  return this->decorators.relinkClass(task);
}

void CodeClassesRegistry::dropDiagnosticList() {
  return this->decorators.dropDiagnosticList();
}

void CodeClassesRegistry::rememberEmittedLateClasses() {
  return this->bodies.rememberEmittedLateClasses();
}

void CodeClassesRegistry::markLateOverrides(SourceFile &program) {
  return this->bodies.markLateOverrides(program);
}

void CodeClassesRegistry::walkConstructorCalls() {
  return this->bodies.walkConstructorCalls();
}

}
