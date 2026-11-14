#include "compiler/semantic/generics_v2/inference/generics_inference_registry.h"

namespace tz {

GenericsInferenceRegistry::GenericsInferenceRegistry(SemanticState &owner)
    : bindingRules(owner),
      boundCalls(owner, this->bindingRules),
      callBindings(owner, this->bindingRules),
      typeArgChecks(owner, this->bindingRules),
      argumentPatterns(owner, this->bindingRules),
      argumentInference(owner, this->bindingRules),
      contextualArguments(owner, this->bindingRules) {}

void GenericsInferenceRegistry::literalShape(const LiteralShape &task) {
  return this->callBindings.literalShape(task);
}

void GenericsInferenceRegistry::checkGenericCall(const CheckGenericCall &task) {
  return this->callBindings.checkGenericCall(task);
}

void GenericsInferenceRegistry::bindGenericCall(const BindGenericCall &task) {
  return this->callBindings.bindGenericCall(task);
}

void GenericsInferenceRegistry::afterExplicitTypeArgs(const AfterExplicitTypeArgs &task) {
  return this->callBindings.afterExplicitTypeArgs(task);
}

void GenericsInferenceRegistry::nextBindingArgument(const NextBindingArgument &task) {
  return this->argumentInference.nextBindingArgument(task);
}

void GenericsInferenceRegistry::inferFromArgument(const InferFromArgument &task) {
  return this->argumentInference.inferFromArgument(task);
}

void GenericsInferenceRegistry::nextPrecheckedField(const NextPrecheckedField &task) {
  return this->argumentInference.nextPrecheckedField(task);
}

void GenericsInferenceRegistry::afterPrecheckedField(const AfterPrecheckedField &task) {
  return this->argumentInference.afterPrecheckedField(task);
}

void GenericsInferenceRegistry::inferPrecheckedField(const InferPrecheckedField &task) {
  return this->argumentInference.inferPrecheckedField(task);
}

void GenericsInferenceRegistry::inferFromListRest(const InferFromListRest &task) {
  return this->argumentInference.inferFromListRest(task);
}

void GenericsInferenceRegistry::retryDeferred(const RetryDeferred &task) {
  return this->argumentInference.retryDeferred(task);
}

void GenericsInferenceRegistry::nextContextualArgument(const NextContextualArgument &task) {
  return this->contextualArguments.nextContextualArgument(task);
}

void GenericsInferenceRegistry::afterContextualPartial(const AfterContextualPartial &task) {
  return this->contextualArguments.afterContextualPartial(task);
}

void GenericsInferenceRegistry::afterDefaultedHint(const AfterDefaultedHint &task) {
  return this->contextualArguments.afterDefaultedHint(task);
}

void GenericsInferenceRegistry::afterContextualArgument(const AfterContextualArgument &task) {
  return this->contextualArguments.afterContextualArgument(task);
}

void GenericsInferenceRegistry::afterBindingTypeArgs(const AfterBindingTypeArgs &task) {
  return this->boundCalls.afterBindingTypeArgs(task);
}

void GenericsInferenceRegistry::afterBoundParams(const AfterBoundParams &task) {
  return this->boundCalls.afterBoundParams(task);
}

void GenericsInferenceRegistry::nextLiteralArgument(const NextLiteralArgument &task) {
  return this->boundCalls.nextLiteralArgument(task);
}

void GenericsInferenceRegistry::afterLiteralHint(const AfterLiteralHint &task) {
  return this->boundCalls.afterLiteralHint(task);
}

void GenericsInferenceRegistry::afterBoundResult(const AfterBoundResult &task) {
  return this->boundCalls.afterBoundResult(task);
}

void GenericsInferenceRegistry::afterBoundArguments(const AfterBoundArguments &task) {
  return this->boundCalls.afterBoundArguments(task);
}

void GenericsInferenceRegistry::finishTypeArgs(const FinishTypeArgs &task) {
  return this->typeArgChecks.finishTypeArgs(task);
}

void GenericsInferenceRegistry::nextTypeArgCheck(const NextTypeArgCheck &task) {
  return this->typeArgChecks.nextTypeArgCheck(task);
}

void GenericsInferenceRegistry::afterTypeArgDefault(const AfterTypeArgDefault &task) {
  return this->typeArgChecks.afterTypeArgDefault(task);
}

void GenericsInferenceRegistry::afterIteratedArgFit(const AfterIteratedArgFit &task) {
  return this->typeArgChecks.afterIteratedArgFit(task);
}

void GenericsInferenceRegistry::afterTypeArgConstraint(const AfterTypeArgConstraint &task) {
  return this->typeArgChecks.afterTypeArgConstraint(task);
}

void GenericsInferenceRegistry::inferFromArguments(const InferFromArguments &task) {
  return this->argumentPatterns.inferFromArguments(task);
}

void GenericsInferenceRegistry::inferArgumentPatterns(const InferArgumentPatterns &task) {
  return this->argumentPatterns.inferArgumentPatterns(task);
}

void GenericsInferenceRegistry::nextFittingArgument(const NextFittingArgument &task) {
  return this->argumentPatterns.nextFittingArgument(task);
}

void GenericsInferenceRegistry::afterFittingDefault(const AfterFittingDefault &task) {
  return this->argumentPatterns.afterFittingDefault(task);
}

void GenericsInferenceRegistry::afterFittingConstraint(const AfterFittingConstraint &task) {
  return this->argumentPatterns.afterFittingConstraint(task);
}

}
