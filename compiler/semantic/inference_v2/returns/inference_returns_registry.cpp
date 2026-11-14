#include "compiler/semantic/inference_v2/returns/inference_returns_registry.h"

namespace tz {

InferenceReturnsRegistry::InferenceReturnsRegistry(SemanticState &owner)
    : returnRules(owner),
      returnBodies(owner, this->returnRules),
      bodyInference(owner, this->returnRules),
      returnRequests(owner, this->returnRules),
      cycleInference(owner, this->returnRules) {}

void InferenceReturnsRegistry::inferReturnType(const InferReturnType &task) {
  return this->returnRequests.inferReturnType(task);
}

void InferenceReturnsRegistry::returnTypeOf(const ReturnTypeOf &task) {
  return this->returnRequests.returnTypeOf(task);
}

void InferenceReturnsRegistry::answerReturnType(const AnswerReturnType &task) {
  return this->returnRequests.answerReturnType(task);
}

void InferenceReturnsRegistry::functionTypeOf(const FunctionTypeOf &task) {
  return this->returnRequests.functionTypeOf(task);
}

void InferenceReturnsRegistry::answerFunctionType(const AnswerFunctionType &task) {
  return this->returnRequests.answerFunctionType(task);
}

void InferenceReturnsRegistry::afterTemplateReturn(const AfterTemplateReturn &task) {
  return this->returnRequests.afterTemplateReturn(task);
}

void InferenceReturnsRegistry::afterTemplateInstance(const AfterTemplateInstance &task) {
  return this->returnRequests.afterTemplateInstance(task);
}

void InferenceReturnsRegistry::setInferredReturn(const SetInferredReturn &task) {
  return this->returnRequests.setInferredReturn(task);
}

void InferenceReturnsRegistry::afterInheritedReturn(const AfterInheritedReturn &task) {
  return this->returnRequests.afterInheritedReturn(task);
}

void InferenceReturnsRegistry::openBodyCheck(const OpenBodyCheck &task) {
  return this->returnBodies.openBodyCheck(task);
}

void InferenceReturnsRegistry::closeBodyCheck(const CloseBodyCheck &task) {
  return this->returnBodies.closeBodyCheck(task);
}

void InferenceReturnsRegistry::enterReturnBody(const EnterReturnBody &task) {
  return this->returnBodies.enterReturnBody(task);
}

void InferenceReturnsRegistry::enterLoadBody() {
  return this->returnBodies.enterLoadBody();
}

void InferenceReturnsRegistry::finishTopLevelEntry(const FinishTopLevelEntry &task) {
  return this->returnBodies.finishTopLevelEntry(task);
}

void InferenceReturnsRegistry::leaveReturnBody() {
  return this->returnBodies.leaveReturnBody();
}

void InferenceReturnsRegistry::leaveLoadBody() {
  return this->returnBodies.leaveLoadBody();
}

void InferenceReturnsRegistry::afterBodyNowInference(const AfterBodyNowInference &task) {
  return this->returnRequests.afterBodyNowInference(task);
}

void InferenceReturnsRegistry::inferFromBody(const InferFromBody &task) {
  return this->bodyInference.inferFromBody(task);
}

void InferenceReturnsRegistry::finishInferFromBody(const FinishInferFromBody &task) {
  return this->bodyInference.finishInferFromBody(task);
}

void InferenceReturnsRegistry::inferCycle(const InferCycle &task) {
  return this->cycleInference.inferCycle(task);
}

void InferenceReturnsRegistry::nextCycleTrial(const NextCycleTrial &task) {
  return this->cycleInference.nextCycleTrial(task);
}

void InferenceReturnsRegistry::afterCycleTrial(const AfterCycleTrial &task) {
  return this->cycleInference.afterCycleTrial(task);
}

void InferenceReturnsRegistry::finishTrialReturn(const FinishTrialReturn &task) {
  return this->cycleInference.finishTrialReturn(task);
}

void InferenceReturnsRegistry::noteInheritedReturn(const NoteInheritedReturn &task) {
  return this->returnRequests.noteInheritedReturn(task);
}

}
