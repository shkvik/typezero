#pragma once

#include "compiler/semantic/inference_v2/returns/body_inference/body_inference.h"
#include "compiler/semantic/inference_v2/returns/cycle_inference/cycle_inference.h"
#include "compiler/semantic/inference_v2/returns/return_bodies/return_bodies.h"
#include "compiler/semantic/inference_v2/returns/return_requests/return_requests.h"
#include "compiler/semantic/inference_v2/returns/return_rules/return_rules.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class InferenceReturnsRegistry {
public:
  explicit InferenceReturnsRegistry(SemanticState &owner);

  void enterLoadBody();
  void leaveLoadBody();
  void leaveReturnBody();
  void inferCycle(const InferCycle &task);
  void returnTypeOf(const ReturnTypeOf &task);
  void inferFromBody(const InferFromBody &task);
  void openBodyCheck(const OpenBodyCheck &task);
  void closeBodyCheck(const CloseBodyCheck &task);
  void functionTypeOf(const FunctionTypeOf &task);
  void nextCycleTrial(const NextCycleTrial &task);
  void afterCycleTrial(const AfterCycleTrial &task);
  void enterReturnBody(const EnterReturnBody &task);
  void inferReturnType(const InferReturnType &task);
  void answerReturnType(const AnswerReturnType &task);
  void finishTrialReturn(const FinishTrialReturn &task);
  void setInferredReturn(const SetInferredReturn &task);
  void answerFunctionType(const AnswerFunctionType &task);
  void afterTemplateReturn(const AfterTemplateReturn &task);
  void finishInferFromBody(const FinishInferFromBody &task);
  void finishTopLevelEntry(const FinishTopLevelEntry &task);
  void noteInheritedReturn(const NoteInheritedReturn &task);
  void afterInheritedReturn(const AfterInheritedReturn &task);
  void afterBodyNowInference(const AfterBodyNowInference &task);
  void afterTemplateInstance(const AfterTemplateInstance &task);

private:
  ReturnRules returnRules;
  ReturnBodies returnBodies;
  BodyInference bodyInference;
  ReturnRequests returnRequests;
  CycleInference cycleInference;
};

}
