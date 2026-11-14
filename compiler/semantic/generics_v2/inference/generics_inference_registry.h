#pragma once

#include "compiler/semantic/generics_v2/inference/argument_inference/argument_inference.h"
#include "compiler/semantic/generics_v2/inference/argument_patterns/argument_patterns.h"
#include "compiler/semantic/generics_v2/inference/binding_rules/binding_rules.h"
#include "compiler/semantic/generics_v2/inference/bound_calls/bound_calls.h"
#include "compiler/semantic/generics_v2/inference/call_bindings/call_bindings.h"
#include "compiler/semantic/generics_v2/inference/contextual_arguments/contextual_arguments.h"
#include "compiler/semantic/generics_v2/inference/type_arg_checks/type_arg_checks.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class GenericsInferenceRegistry {
public:
  explicit GenericsInferenceRegistry(SemanticState &owner);

  void literalShape(const LiteralShape &task);
  void retryDeferred(const RetryDeferred &task);
  void finishTypeArgs(const FinishTypeArgs &task);
  void bindGenericCall(const BindGenericCall &task);
  void afterBoundParams(const AfterBoundParams &task);
  void afterBoundResult(const AfterBoundResult &task);
  void afterLiteralHint(const AfterLiteralHint &task);
  void checkGenericCall(const CheckGenericCall &task);
  void nextTypeArgCheck(const NextTypeArgCheck &task);
  void inferFromArgument(const InferFromArgument &task);
  void inferFromListRest(const InferFromListRest &task);
  void afterDefaultedHint(const AfterDefaultedHint &task);
  void inferFromArguments(const InferFromArguments &task);
  void afterBoundArguments(const AfterBoundArguments &task);
  void afterFittingDefault(const AfterFittingDefault &task);
  void afterIteratedArgFit(const AfterIteratedArgFit &task);
  void afterTypeArgDefault(const AfterTypeArgDefault &task);
  void nextBindingArgument(const NextBindingArgument &task);
  void nextFittingArgument(const NextFittingArgument &task);
  void nextLiteralArgument(const NextLiteralArgument &task);
  void nextPrecheckedField(const NextPrecheckedField &task);
  void afterBindingTypeArgs(const AfterBindingTypeArgs &task);
  void afterPrecheckedField(const AfterPrecheckedField &task);
  void inferPrecheckedField(const InferPrecheckedField &task);
  void afterExplicitTypeArgs(const AfterExplicitTypeArgs &task);
  void inferArgumentPatterns(const InferArgumentPatterns &task);
  void afterContextualPartial(const AfterContextualPartial &task);
  void afterFittingConstraint(const AfterFittingConstraint &task);
  void afterTypeArgConstraint(const AfterTypeArgConstraint &task);
  void nextContextualArgument(const NextContextualArgument &task);
  void afterContextualArgument(const AfterContextualArgument &task);

private:
  BindingRules bindingRules;
  BoundCalls boundCalls;
  CallBindings callBindings;
  TypeArgChecks typeArgChecks;
  ArgumentPatterns argumentPatterns;
  ArgumentInference argumentInference;
  ContextualArguments contextualArguments;
};

}
