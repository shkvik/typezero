#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/semantic/semantic_state/flow_state/flow_state_types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include <vector>

namespace tz {

class PredicateStore {
public:
  void enterPredicateBuild(PredicateBuild build);
  PredicateBuild leavePredicateBuild();
  bool isBuildingPredicate() const;
  const PredicateBuild &getPredicateBuild() const;
  void updatePredicateBuild(Flow whenTrue, Flow whenFalse);
  void rememberPredicate(const FuncSymbol *function, Predicate predicate);
  const Predicate *findPredicate(const FuncSymbol *function) const;
  void markSummarizing(const FuncSymbol *function);
  bool isSummarizing(const FuncSymbol *function) const;

private:
  std::vector<PredicateBuild> builds;
  llvm::DenseMap<const FuncSymbol *, Predicate> predicates;
  llvm::SmallPtrSet<const FuncSymbol *, 8> summarizing;
};

}
