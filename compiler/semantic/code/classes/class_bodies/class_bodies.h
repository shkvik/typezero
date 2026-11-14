#pragma once

#include "compiler/semantic/code/classes/class_bodies/class_body_walks/class_body_walks.h"
#include "compiler/semantic/code/classes/class_bodies/class_statements/class_statements.h"
#include "compiler/semantic/code/classes/class_bodies/late_class_walks/late_class_walks.h"
#include "compiler/semantic/code/classes/class_facts/class_facts.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class ClassBodies {
public:
  ClassBodies(SemanticState &owner, ClassFacts &sharedFacts);

  void nextLateClass();
  void leaveStaticFields();
  void pushLateClassTasks();
  void leaveDeclaringClass();
  void walkConstructorCalls();
  void rememberEmittedLateClasses();
  void declareThis(const DeclareThis &task);
  void markLateOverrides(SourceFile &program);
  void afterLateBase(const AfterLateBase &task);
  void walkLateClass(const WalkLateClass &task);
  void checkClassStmt(const CheckClassStmt &task);
  void checkClassExpr(const CheckClassExpr &task);
  void checkFieldsSet(const CheckFieldsSet &task);
  void finishClassExpr(const FinishClassExpr &task);
  void walkStaticField(const WalkStaticField &task);
  void finishLateClass(const FinishLateClass &task);
  void walkClassBodies(const WalkClassBodies &task);
  void walkFieldStarts(const WalkFieldStarts &task);
  void walkClassMethod(const WalkClassMethod &task);
  void walkStaticFields(const WalkStaticFields &task);
  void afterStaticField(const AfterStaticField &task);
  void walkClassFunction(const WalkClassFunction &task);
  void finishClassBodies(const FinishClassBodies &task);
  void afterLateClassExpr(const AfterLateClassExpr &task);
  void finishLateClassWalk(const FinishLateClassWalk &task);
  void bindOuterTypeParams(const BindOuterTypeParams &task);
  void checkConstructorArgs(const CheckConstructorArgs &task);
  void afterClassMethodReturn(const AfterClassMethodReturn &task);
  void resolveConstructorType(const ResolveConstructorType &task);
  void afterConstructorTypeArgument(const AfterConstructorTypeArgument &task);

private:
  ClassBodyWalks classBodyWalks;
  LateClassWalks lateClassWalks;
  ClassStatements classStatements;
};

}
