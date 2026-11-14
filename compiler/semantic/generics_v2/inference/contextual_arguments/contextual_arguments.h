#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/generics_v2/inference/binding_rules/binding_rules.h"
#include "compiler/semantic/generics_v2/inference/generics_inference_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class ContextualArguments {
public:
  ContextualArguments(SemanticState &owner, BindingRules &sharedBindingRules);

  void afterDefaultedHint(const AfterDefaultedHint &task);
  void afterContextualPartial(const AfterContextualPartial &task);
  void nextContextualArgument(const NextContextualArgument &task);
  void afterContextualArgument(const AfterContextualArgument &task);

private:
  using ResultCallbackParam = Result<const Type *, GenericsInferenceRegistryCallbackParamError>;

  void adoptCandidates(GenericBindingDraft &draft, const CandidateInference &more);
  void pushCallbackValue(Expr **slot, FuncSymbol *other, const Signature &signature);
  void walkContextualNow(GenericBindingDraft draft, size_t position, const Type *hint);
  void walkCallbackParams(GenericBindingDraft &draft, size_t index, const Type *function);
  void walkCallbackArgument(GenericBindingDraft draft, size_t position, const Type *function);
  void walkArgumentWithDefaults(GenericBindingDraft draft, size_t position, const Type *partial);
  void walkCallbackParam(GenericBindingDraft &draft, size_t index, const Type *callbackParam,
                         size_t position);


  bool fillTypeDefaults(GenericBindingDraft &draft);


  const Type *typeDefault(const FuncSymbol *generic, size_t index) const;
  Signature callbackSignature(llvm::ArrayRef<const Type *> openParams, const Type *function) const;
  ResultCallbackParam callbackParam(const FuncSymbol *generic, const Type *param, const Type *bound,
                                    bool isMentioned, const FunctionExpr *functionExpr,
                                    size_t position) const;

  SemanticState &state;
  BindingRules &bindingRules;
};

}
