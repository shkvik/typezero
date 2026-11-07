#pragma once

#include "compiler/pipeline/check_program/check_state/flow_state/flow_state_types.h"
#include "compiler/program/ast/ast.h"
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
  void rememberPredicate(const FuncSymbol *fn, Predicate predicate);
  const Predicate *findPredicate(const FuncSymbol *fn) const;
  void markSummarizing(const FuncSymbol *fn);
  bool isSummarizing(const FuncSymbol *fn) const;

private:
  std::vector<PredicateBuild> builds;
  llvm::DenseMap<const FuncSymbol *, Predicate> predicates;
  llvm::SmallPtrSet<const FuncSymbol *, 8> summarizing;
};

}
