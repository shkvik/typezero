#include "compiler/semantic/code/functions/code_functions_registry.h"

namespace tz {

CodeFunctionsRegistry::CodeFunctionsRegistry(SemanticState &owner)
    : functionRules(owner),
      closureBodies(owner, this->functionRules),
      functionBodies(owner),
      arrayCallbacks(owner),
      functionMethods(owner),
      closureSignatures(owner),
      callbackArguments(owner, this->functionRules),
      functionStatements(owner) {}

void CodeFunctionsRegistry::walkFunctionBody(const WalkFunctionBody &task) {
  return this->functionBodies.walkFunctionBody(task);
}

void CodeFunctionsRegistry::afterBodyTypeParams(const AfterBodyTypeParams &task) {
  return this->functionBodies.afterBodyTypeParams(task);
}

void CodeFunctionsRegistry::afterBodyThis(const AfterBodyThis &task) {
  return this->functionBodies.afterBodyThis(task);
}

void CodeFunctionsRegistry::nextBodyParam(const NextBodyParam &task) {
  return this->functionBodies.nextBodyParam(task);
}

void CodeFunctionsRegistry::afterDefaultedParam(const AfterDefaultedParam &task) {
  return this->functionBodies.afterDefaultedParam(task);
}

void CodeFunctionsRegistry::enterErrorWatch() {
  return this->functionBodies.enterErrorWatch();
}

void CodeFunctionsRegistry::afterBodyStatements(const AfterBodyStatements &task) {
  return this->functionBodies.afterBodyStatements(task);
}

void CodeFunctionsRegistry::leaveErrorWatch() {
  return this->functionBodies.leaveErrorWatch();
}

void CodeFunctionsRegistry::afterPredicateEnd(const AfterPredicateEnd &task) {
  return this->functionBodies.afterPredicateEnd(task);
}

void CodeFunctionsRegistry::finishFunctionBody(const FinishFunctionBody &task) {
  return this->functionBodies.finishFunctionBody(task);
}

void CodeFunctionsRegistry::walkInferredBody(const WalkInferredBody &task) {
  return this->functionBodies.walkInferredBody(task);
}

void CodeFunctionsRegistry::walkFunctionFromScratch(const WalkFunctionFromScratch &task) {
  return this->functionBodies.walkFunctionFromScratch(task);
}

void CodeFunctionsRegistry::declareArgumentList(const DeclareArgumentList &task) {
  return this->functionBodies.declareArgumentList(task);
}

void CodeFunctionsRegistry::rememberArgumentList(const RememberArgumentList &task) {
  return this->functionBodies.rememberArgumentList(task);
}

void CodeFunctionsRegistry::walkStatements(const WalkStatements &task) {
  return this->functionStatements.walkStatements(task);
}

void CodeFunctionsRegistry::afterBlockTypes(const AfterBlockTypes &task) {
  return this->functionStatements.afterBlockTypes(task);
}

void CodeFunctionsRegistry::enterStatements(const EnterStatements &task) {
  return this->functionStatements.enterStatements(task);
}

void CodeFunctionsRegistry::nextStatement(const NextStatement &task) {
  return this->functionStatements.nextStatement(task);
}

void CodeFunctionsRegistry::afterStatement(const AfterStatement &task) {
  return this->functionStatements.afterStatement(task);
}

void CodeFunctionsRegistry::groupMember(const GroupMember &task) {
  return this->functionStatements.groupMember(task);
}

void CodeFunctionsRegistry::afterGroupMemberReturn(const AfterGroupMemberReturn &task) {
  return this->functionStatements.afterGroupMemberReturn(task);
}

void CodeFunctionsRegistry::groupUseOrder(const GroupUseOrder &task) {
  return this->functionStatements.groupUseOrder(task);
}

void CodeFunctionsRegistry::leaveGroup() {
  return this->functionStatements.leaveGroup();
}

void CodeFunctionsRegistry::noteUse(const NoteUse &task) {
  return this->functionStatements.noteUse(task);
}

void CodeFunctionsRegistry::checkFunctionExpr(const CheckFunctionExpr &task) {
  return this->closureSignatures.checkFunctionExpr(task);
}

void CodeFunctionsRegistry::nextClosureParam(const NextClosureParam &task) {
  return this->closureSignatures.nextClosureParam(task);
}

void CodeFunctionsRegistry::afterClosureParamType(const AfterClosureParamType &task) {
  return this->closureSignatures.afterClosureParamType(task);
}

void CodeFunctionsRegistry::afterClosureReturnType(const AfterClosureReturnType &task) {
  return this->closureSignatures.afterClosureReturnType(task);
}

void CodeFunctionsRegistry::afterClosureReturn(const AfterClosureReturn &task) {
  return this->closureBodies.afterClosureReturn(task);
}

void CodeFunctionsRegistry::startClosureBody(const StartClosureBody &task) {
  return this->closureBodies.startClosureBody(task);
}

void CodeFunctionsRegistry::leaveInitializerClosure() {
  return this->closureBodies.leaveInitializerClosure();
}

void CodeFunctionsRegistry::declareOwnName(const DeclareOwnName &task) {
  return this->closureBodies.declareOwnName(task);
}

void CodeFunctionsRegistry::keepConstantFlow(const KeepConstantFlow &task) {
  return this->closureBodies.keepConstantFlow(task);
}

void CodeFunctionsRegistry::afterClosureType(const AfterClosureType &task) {
  return this->closureBodies.afterClosureType(task);
}

void CodeFunctionsRegistry::checkFunctionValue(const CheckFunctionValue &task) {
  return this->closureBodies.checkFunctionValue(task);
}

void CodeFunctionsRegistry::checkFunctionArgSlot(const CheckFunctionArgSlot &task) {
  return this->callbackArguments.checkFunctionArgSlot(task);
}

void CodeFunctionsRegistry::afterLiteralCallback(const AfterLiteralCallback &task) {
  return this->callbackArguments.afterLiteralCallback(task);
}

void CodeFunctionsRegistry::leaveCallback() {
  return this->callbackArguments.leaveCallback();
}

void CodeFunctionsRegistry::afterCallbackValue(const AfterCallbackValue &task) {
  return this->callbackArguments.afterCallbackValue(task);
}

void CodeFunctionsRegistry::checkCallbackMethod(const CheckCallbackMethod &task) {
  return this->arrayCallbacks.checkCallbackMethod(task);
}

void CodeFunctionsRegistry::afterExtraCallbackArgs(const AfterExtraCallbackArgs &task) {
  return this->arrayCallbacks.afterExtraCallbackArgs(task);
}

void CodeFunctionsRegistry::afterReduceInit(const AfterReduceInit &task) {
  return this->arrayCallbacks.afterReduceInit(task);
}

void CodeFunctionsRegistry::finishCallbackMethod(const FinishCallbackMethod &task) {
  return this->arrayCallbacks.finishCallbackMethod(task);
}

void CodeFunctionsRegistry::checkArrayCallbackResult(const CheckArrayCallbackResult &task) {
  return this->arrayCallbacks.checkArrayCallbackResult(task);
}

void CodeFunctionsRegistry::checkThisCall(const CheckThisCall &task) {
  return this->functionMethods.checkThisCall(task);
}

void CodeFunctionsRegistry::checkListMethod(const CheckListMethod &task) {
  return this->functionMethods.checkListMethod(task);
}

void CodeFunctionsRegistry::findListPart(const FindListPart &task) {
  return this->functionMethods.findListPart(task);
}

void CodeFunctionsRegistry::checkListArguments(const CheckListArguments &task) {
  return this->functionMethods.checkListArguments(task);
}

void CodeFunctionsRegistry::nextListArgument(const NextListArgument &task) {
  return this->functionMethods.nextListArgument(task);
}

void CodeFunctionsRegistry::afterListArgument(const AfterListArgument &task) {
  return this->functionMethods.afterListArgument(task);
}

void CodeFunctionsRegistry::startModuleBodies(const StartModuleBodies &task) {
  return this->functionBodies.startModuleBodies(task);
}

void CodeFunctionsRegistry::startLiftedFunction(const StartLiftedFunction &task) {
  return this->functionBodies.startLiftedFunction(task);
}

void CodeFunctionsRegistry::walkTopFunction(const WalkTopFunction &task) {
  return this->functionBodies.walkTopFunction(task);
}

void CodeFunctionsRegistry::afterTopReturnType(const AfterTopReturnType &task) {
  return this->functionBodies.afterTopReturnType(task);
}

}
