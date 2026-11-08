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
  return !this->builds.empty() && this->builds.back().fn;
}

const PredicateBuild &PredicateStore::getPredicateBuild() const {
  return this->builds.back();
}

void PredicateStore::updatePredicateBuild(Flow whenTrue, Flow whenFalse) {
  this->builds.back().whenTrue = std::move(whenTrue);
  this->builds.back().whenFalse = std::move(whenFalse);
}

void PredicateStore::rememberPredicate(const FuncSymbol *fn, Predicate predicate) {
  this->predicates[fn] = std::move(predicate);
}

const Predicate *PredicateStore::findPredicate(const FuncSymbol *fn) const {
  auto found = this->predicates.find(fn);
  if (found == this->predicates.end()) {
    return nullptr;
  }

  return &found->second;
}

void PredicateStore::markSummarizing(const FuncSymbol *fn) {
  this->summarizing.insert(fn);
}

bool PredicateStore::isSummarizing(const FuncSymbol *fn) const {
  return this->summarizing.contains(fn);
}

}
