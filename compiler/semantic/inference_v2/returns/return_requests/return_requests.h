#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/inference_v2/returns/inference_returns_registry_errors.h"
#include "compiler/semantic/inference_v2/returns/return_rules/return_rules.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>

namespace tz {

class ReturnRequests {
public:
  ReturnRequests(SemanticState &owner, ReturnRules &sharedReturnRules);

  void returnTypeOf(const ReturnTypeOf &task);
  void functionTypeOf(const FunctionTypeOf &task);
  void inferReturnType(const InferReturnType &task);
  void answerReturnType(const AnswerReturnType &task);
  void setInferredReturn(const SetInferredReturn &task);
  void answerFunctionType(const AnswerFunctionType &task);
  void afterTemplateReturn(const AfterTemplateReturn &task);
  void noteInheritedReturn(const NoteInheritedReturn &task);
  void afterInheritedReturn(const AfterInheritedReturn &task);
  void afterBodyNowInference(const AfterBodyNowInference &task);
  void afterTemplateInstance(const AfterTemplateInstance &task);

private:
  using ResultNotCalledEarly = Result<const FuncSymbol *, InferenceReturnsRegistryEarlyCallError>;

  void startInference(FuncSymbol *function);
  void walkBodyNow(const WalkBodyNow &task);
  void startBodyInference(FuncSymbol *function);
  void pushBodyCheck(FuncSymbol *function, CheckTask work);
  void inheritReturn(FuncSymbol *function, FuncSymbol *source);


  bool isTemplateCopy(const FunctionDecl *functionDecl);


  std::optional<const Type *> settleReturn(FuncSymbol *function);
  ResultNotCalledEarly earlyCall(const FuncSymbol *function) const;

  SemanticState &state;
  ReturnRules &returnRules;
};

}
