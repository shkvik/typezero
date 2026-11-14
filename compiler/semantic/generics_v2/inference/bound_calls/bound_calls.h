#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/generics_v2/inference/binding_rules/binding_rules.h"
#include "compiler/semantic/generics_v2/inference/generics_inference_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <vector>

namespace tz {

class BoundCalls {
public:
  BoundCalls(SemanticState &owner, BindingRules &sharedBindingRules);

  void afterBoundParams(const AfterBoundParams &task);
  void afterBoundResult(const AfterBoundResult &task);
  void afterLiteralHint(const AfterLiteralHint &task);
  void afterBoundArguments(const AfterBoundArguments &task);
  void nextLiteralArgument(const NextLiteralArgument &task);
  void afterBindingTypeArgs(const AfterBindingTypeArgs &task);

private:
  enum class GrowthPlace {
    TOP,
    INSIDE,
  };

  using ResultArgNotGrowing = Result<const Type *, GenericsInferenceRegistryNotGrowingError>;

  void instantiateCall(GenericBindingDraft draft);
  void walkGrowingArgs(GenericBindingDraft &draft);
  void deliverBinding(const GenericBindingDraft &draft);
  void substituteBoundResult(GenericBindingDraft draft);
  void targetGenericCall(const GenericBindingDraft &draft);
  void walkUncheckedArguments(const GenericBindingDraft &draft);
  void checkBoundArguments(GenericBindingDraft draft, const Type *result);
  void takeExactClass(CallExpr *call, size_t index, size_t paramCount,
                      std::vector<const Type *> &exact);


  bool isGrowing(const Type *type, llvm::ArrayRef<const Type *> params) const;
  bool isGenericClassParam(const FuncSymbol *generic, const Type *param) const;
  bool isConcreteCall(const FuncSymbol *generic, llvm::ArrayRef<const Type *> bound) const;
  bool isGrowingAt(const Type *type, llvm::ArrayRef<const Type *> params, GrowthPlace place) const;


  std::vector<const CastExpr *> implicitCasts(const Expr *arg) const;
  std::vector<const Type *> exactClassArgs(CallExpr *call, const FuncSymbol *generic);
  ResultArgNotGrowing notGrowing(const FuncSymbol *generic, const Type *bound,
                                 const Type *param) const;

  SemanticState &state;
  BindingRules &bindingRules;
};

}
