#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/inference_v2/returns/inference_returns_registry_errors.h"
#include "compiler/semantic/inference_v2/returns/return_rules/return_rules.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class CycleInference {
public:
  CycleInference(SemanticState &owner, ReturnRules &sharedReturnRules);

  void inferCycle(const InferCycle &task);
  void nextCycleTrial(const NextCycleTrial &task);
  void afterCycleTrial(const AfterCycleTrial &task);
  void finishTrialReturn(const FinishTrialReturn &task);

private:
  using ResultCycleTypeKnown = Result<const Type *, InferenceReturnsRegistryCycleTypeError>;

  void startTrialReturn(FuncSymbol *function);
  void finishCycle(const CycleDraft &draft, CycleStep settled);
  void walkCycleTypes(llvm::ArrayRef<FuncSymbol *> cycle, bool isFailed);


  CycleStep nextAssumption(FuncSymbol *function, const Type *found);
  ResultCycleTypeKnown cycleType(const FuncSymbol *function, int cycleSize, bool isFailed) const;

  SemanticState &state;
  ReturnRules &returnRules;
};

}
