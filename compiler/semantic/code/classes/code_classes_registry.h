#pragma once

#include "compiler/semantic/code/classes/class_bodies/class_bodies.h"
#include "compiler/semantic/code/classes/class_calls/class_calls.h"
#include "compiler/semantic/code/classes/class_decorators/class_decorators.h"
#include "compiler/semantic/code/classes/class_facts/class_facts.h"
#include "compiler/semantic/code/classes/class_members/class_members.h"
#include "compiler/semantic/code/classes/overload_choice/overload_choice.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class CodeClassesRegistry {
public:
  explicit CodeClassesRegistry(SemanticState &owner);

  void nextLateClass();
  void leaveClassContext();
  void leaveStaticFields();
  void dropDiagnosticList();
  void pushLateClassTasks();
  void leaveDeclaringClass();
  void walkConstructorCalls();
  void rememberEmittedLateClasses();
  void decorate(const Decorate &task);
  void checkThis(const CheckThis &task);
  void declareThis(const DeclareThis &task);
  void relinkClass(const RelinkClass &task);
  void markLateOverrides(SourceFile &program);
  void rankVariants(const RankVariants &task);
  void afterLateBase(const AfterLateBase &task);
  void checkClassNew(const CheckClassNew &task);
  void checkValueNew(const CheckValueNew &task);
  void chooseVariant(const ChooseVariant &task);
  void decorateClass(const DecorateClass &task);
  void walkLateClass(const WalkLateClass &task);
  void checkClassExpr(const CheckClassExpr &task);
  void checkClassStmt(const CheckClassStmt &task);
  void checkFieldsSet(const CheckFieldsSet &task);
  void checkSuperCall(const CheckSuperCall &task);
  void classValueType(const ClassValueType &task);
  void finishClassNew(const FinishClassNew &task);
  void afterBoundTypes(const AfterBoundTypes &task);
  void checkClassValue(const CheckClassValue &task);
  void checkMethodCall(const CheckMethodCall &task);
  void finishClassExpr(const FinishClassExpr &task);
  void finishLateClass(const FinishLateClass &task);
  void finishSuperCall(const FinishSuperCall &task);
  void instanceOfRight(const InstanceOfRight &task);
  void walkClassBodies(const WalkClassBodies &task);
  void walkClassMethod(const WalkClassMethod &task);
  void walkFieldStarts(const WalkFieldStarts &task);
  void walkStaticField(const WalkStaticField &task);
  void afterStaticField(const AfterStaticField &task);
  void afterSuperObject(const AfterSuperObject &task);
  void checkClassMember(const CheckClassMember &task);
  void checkFieldTarget(const CheckFieldTarget &task);
  void nextReflectedArg(const NextReflectedArg &task);
  void walkStaticFields(const WalkStaticFields &task);
  void afterMethodChoice(const AfterMethodChoice &task);
  void afterReflectedArg(const AfterReflectedArg &task);
  void checkOverloadCall(const CheckOverloadCall &task);
  void checkStaticMember(const CheckStaticMember &task);
  void checkStaticTarget(const CheckStaticTarget &task);
  void finishClassBodies(const FinishClassBodies &task);
  void nextRankedVariant(const NextRankedVariant &task);
  void walkClassFunction(const WalkClassFunction &task);
  void afterGenericCallee(const AfterGenericCallee &task);
  void afterGenericParams(const AfterGenericParams &task);
  void afterLateClassExpr(const AfterLateClassExpr &task);
  void afterMethodCallTry(const AfterMethodCallTry &task);
  void afterClassRefStatic(const AfterClassRefStatic &task);
  void afterInstanceOfLeft(const AfterInstanceOfLeft &task);
  void afterMethodVariants(const AfterMethodVariants &task);
  void afterOverloadChoice(const AfterOverloadChoice &task);
  void afterStaticVariants(const AfterStaticVariants &task);
  void afterValueNewCallee(const AfterValueNewCallee &task);
  void bindOuterTypeParams(const BindOuterTypeParams &task);
  void checkClassRefMember(const CheckClassRefMember &task);
  void checkOverloadMember(const CheckOverloadMember &task);
  void finishLateClassWalk(const FinishLateClassWalk &task);
  void afterInstanceOfRight(const AfterInstanceOfRight &task);
  void afterPlainMethodType(const AfterPlainMethodType &task);
  void afterSuperEmitterTry(const AfterSuperEmitterTry &task);
  void checkClassStaticCall(const CheckClassStaticCall &task);
  void checkConstructorArgs(const CheckConstructorArgs &task);
  void checkSuperMethodCall(const CheckSuperMethodCall &task);
  void makeMethodDescriptor(const MakeMethodDescriptor &task);
  void nextAppliedDecorator(const NextAppliedDecorator &task);
  void nextHoistedDecorator(const NextHoistedDecorator &task);
  void afterClassFieldCallee(const AfterClassFieldCallee &task);
  void afterDecoratorApplied(const AfterDecoratorApplied &task);
  void afterNameFunctionType(const AfterNameFunctionType &task);
  void afterOverloadVariants(const AfterOverloadVariants &task);
  void afterClassMethodReturn(const AfterClassMethodReturn &task);
  void afterConstructorChoice(const AfterConstructorChoice &task);
  void afterEventsFunctionTry(const AfterEventsFunctionTry &task);
  void afterGenericMethodCall(const AfterGenericMethodCall &task);
  void afterInferredArguments(const AfterInferredArguments &task);
  void afterStaticFieldCallee(const AfterStaticFieldCallee &task);
  void afterSuperMethodChoice(const AfterSuperMethodChoice &task);
  void checkConstructorAccess(const CheckConstructorAccess &task);
  void resolveConstructorType(const ResolveConstructorType &task);
  void afterDescriptorDeclared(const AfterDescriptorDeclared &task);
  void afterMemberFunctionType(const AfterMemberFunctionType &task);
  void afterStaticMethodChoice(const AfterStaticMethodChoice &task);
  void checkInstanceOfOperator(const CheckInstanceOfOperator &task);
  void declareHoistedDecorator(const DeclareHoistedDecorator &task);
  void afterConstructorTypeArgument(const AfterConstructorTypeArgument &task);
  void afterParentConstructorChoice(const AfterParentConstructorChoice &task);

private:
  ClassFacts facts;
  ClassCalls calls;
  ClassBodies bodies;
  ClassMembers members;
  OverloadChoice overloads;
  ClassDecorators decorators;
};

}
