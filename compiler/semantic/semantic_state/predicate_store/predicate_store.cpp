#include "compiler/semantic/semantic_state/predicate_store/predicate_store.h"

namespace tz {

void PredicateStore::enterPredicateBuild(PredicateBuild build) {
  this->builds.push_back(std::move(build));
}

PredicateBuild PredicateStore::leavePredicateBuild() {
  PredicateBuild build = std::move(this->builds.back());
  this->builds.pop_back();

  return build;
}

bool PredicateStore::isBuildingPredicate() const {
  return !this->builds.empty() && this->builds.back().fn != nullptr;
}

const PredicateBuild &PredicateStore::getPredicateBuild() const {
  return this->builds.back();
}

void PredicateStore::updatePredicateBuild(Flow whenTrue, Flow whenFalse) {
  this->builds.back().whenTrue = std::move(whenTrue);
  this->builds.back().whenFalse = std::move(whenFalse);
}

void PredicateStore::rememberPredicate(const FuncSymbol *function, Predicate predicate) {
  this->predicates[function] = std::move(predicate);
}

const Predicate *PredicateStore::findPredicate(const FuncSymbol *function) const {
  auto found = this->predicates.find(function);
  if (found == this->predicates.end()) {
    return nullptr;
  }

  return &found->second;
}

void PredicateStore::markSummarizing(const FuncSymbol *function) {
  this->summarizing.insert(function);
}

bool PredicateStore::isSummarizing(const FuncSymbol *function) const {
  return this->summarizing.contains(function);
}

}
