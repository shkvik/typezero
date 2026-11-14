#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/generics_v2/inference/binding_rules/binding_rules.h"
#include "compiler/semantic/generics_v2/inference/generics_inference_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <string>

namespace tz {

class TypeArgChecks {
public:
  TypeArgChecks(SemanticState &owner, BindingRules &sharedBindingRules);

  void finishTypeArgs(const FinishTypeArgs &task);
  void nextTypeArgCheck(const NextTypeArgCheck &task);
  void afterIteratedArgFit(const AfterIteratedArgFit &task);
  void afterTypeArgDefault(const AfterTypeArgDefault &task);
  void afterTypeArgConstraint(const AfterTypeArgConstraint &task);

private:
  using ResultIterableMatch = Result<const Type *, GenericsInferenceRegistryIterableFitsError>;
  using ResultConstraintMet = Result<const Type *, GenericsInferenceRegistryConstraintMetError>;
  using ResultParamInferred = Result<const Type *, GenericsInferenceRegistryParamInferredError>;

  void acceptTypeArg(TypeArgsDraft draft);
  void finishTypeArg(TypeArgsDraft draft);
  void rejectTypeArg(TypeArgsDraft draft);
  void deliverTypeArgs(const TypeArgsDraft &draft);


  bool isMentioningOpen(const Type *type, llvm::ArrayRef<const Type *> params,
                        llvm::ArrayRef<const Type *> bound) const;


  std::string sampleArgs(llvm::ArrayRef<const Type *> params) const;
  ResultParamInferred paramInferred(const FuncSymbol *generic, const Type *bound, size_t index,
                                    TypeArgUse use) const;
  ResultIterableMatch iterableFits(const FuncSymbol *generic, const Type *bound, const Type *given,
                                   const Type *wanted) const;
  ResultConstraintMet constraintMet(const FuncSymbol *generic, llvm::ArrayRef<const Type *> bound,
                                    size_t index, bool isSatisfied) const;

  SemanticState &state;
  BindingRules &bindingRules;
};

}
