#pragma once

#include "compiler/semantic/code/functions/array_callbacks/array_callbacks.h"
#include "compiler/semantic/code/functions/callback_arguments/callback_arguments.h"
#include "compiler/semantic/code/functions/closure_bodies/closure_bodies.h"
#include "compiler/semantic/code/functions/closure_signatures/closure_signatures.h"
#include "compiler/semantic/code/functions/function_bodies/function_bodies.h"
#include "compiler/semantic/code/functions/function_methods/function_methods.h"
#include "compiler/semantic/code/functions/function_rules/function_rules.h"
#include "compiler/semantic/code/functions/function_statements/function_statements.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class CodeFunctionsRegistry {
public:
  explicit CodeFunctionsRegistry(SemanticState &owner);

  void leaveGroup();
  void leaveCallback();
  void enterErrorWatch();
  void leaveErrorWatch();
  void leaveInitializerClosure();
  void noteUse(const NoteUse &task);
  void groupMember(const GroupMember &task);
  void findListPart(const FindListPart &task);
  void afterBodyThis(const AfterBodyThis &task);
  void nextBodyParam(const NextBodyParam &task);
  void nextStatement(const NextStatement &task);
  void groupUseOrder(const GroupUseOrder &task);
  void checkThisCall(const CheckThisCall &task);
  void walkStatements(const WalkStatements &task);
  void afterStatement(const AfterStatement &task);
  void declareOwnName(const DeclareOwnName &task);
  void afterBlockTypes(const AfterBlockTypes &task);
  void enterStatements(const EnterStatements &task);
  void afterReduceInit(const AfterReduceInit &task);
  void checkListMethod(const CheckListMethod &task);
  void walkTopFunction(const WalkTopFunction &task);
  void walkFunctionBody(const WalkFunctionBody &task);
  void walkInferredBody(const WalkInferredBody &task);
  void nextClosureParam(const NextClosureParam &task);
  void startClosureBody(const StartClosureBody &task);
  void keepConstantFlow(const KeepConstantFlow &task);
  void afterClosureType(const AfterClosureType &task);
  void nextListArgument(const NextListArgument &task);
  void afterPredicateEnd(const AfterPredicateEnd &task);
  void checkFunctionExpr(const CheckFunctionExpr &task);
  void afterListArgument(const AfterListArgument &task);
  void startModuleBodies(const StartModuleBodies &task);
  void finishFunctionBody(const FinishFunctionBody &task);
  void afterClosureReturn(const AfterClosureReturn &task);
  void checkFunctionValue(const CheckFunctionValue &task);
  void afterCallbackValue(const AfterCallbackValue &task);
  void checkListArguments(const CheckListArguments &task);
  void afterTopReturnType(const AfterTopReturnType &task);
  void afterBodyTypeParams(const AfterBodyTypeParams &task);
  void afterDefaultedParam(const AfterDefaultedParam &task);
  void afterBodyStatements(const AfterBodyStatements &task);
  void declareArgumentList(const DeclareArgumentList &task);
  void checkCallbackMethod(const CheckCallbackMethod &task);
  void startLiftedFunction(const StartLiftedFunction &task);
  void rememberArgumentList(const RememberArgumentList &task);
  void checkFunctionArgSlot(const CheckFunctionArgSlot &task);
  void afterLiteralCallback(const AfterLiteralCallback &task);
  void finishCallbackMethod(const FinishCallbackMethod &task);
  void afterClosureParamType(const AfterClosureParamType &task);
  void afterGroupMemberReturn(const AfterGroupMemberReturn &task);
  void afterClosureReturnType(const AfterClosureReturnType &task);
  void afterExtraCallbackArgs(const AfterExtraCallbackArgs &task);
  void walkFunctionFromScratch(const WalkFunctionFromScratch &task);
  void checkArrayCallbackResult(const CheckArrayCallbackResult &task);

private:
  FunctionRules functionRules;
  ClosureBodies closureBodies;
  FunctionBodies functionBodies;
  ArrayCallbacks arrayCallbacks;
  FunctionMethods functionMethods;
  ClosureSignatures closureSignatures;
  CallbackArguments callbackArguments;
  FunctionStatements functionStatements;
};

}
