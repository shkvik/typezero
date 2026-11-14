#include "compiler/semantic/semantic_state/v2/flow/predicate_context/predicate_context.h"

namespace tz {

void PredicateContext::enterPredicateBuild(PredicateBuild build) {
  this->builds.push_back(std::move(build));
}

PredicateBuild PredicateContext::leavePredicateBuild() {
  PredicateBuild build = std::move(this->builds.back());
  this->builds.pop_back();

  return build;
}

bool PredicateContext::isBuildingPredicate() const {
  return !this->builds.empty() && this->builds.back().fn != nullptr;
}

const PredicateBuild &PredicateContext::getPredicateBuild() const {
  return this->builds.back();
}

void PredicateContext::updatePredicateBuild(Flow whenTrue, Flow whenFalse) {
  this->builds.back().whenTrue = std::move(whenTrue);
  this->builds.back().whenFalse = std::move(whenFalse);
}

void PredicateContext::rememberPredicate(const FuncSymbol *function, Predicate predicate) {
  this->predicates[function] = std::move(predicate);
}

const Predicate *PredicateContext::findPredicate(const FuncSymbol *function) const {
  auto found = this->predicates.find(function);
  if (found == this->predicates.end()) {
    return nullptr;
  }

  return &found->second;
}

void PredicateContext::markSummarizing(const FuncSymbol *function) {
  this->summarizing.insert(function);
}

bool PredicateContext::isSummarizing(const FuncSymbol *function) const {
  return this->summarizing.contains(function);
}

}
