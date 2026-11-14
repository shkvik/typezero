#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/semantic/semantic_state/flow_state/flow_state_types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include <vector>

namespace tz {

class PredicateContext {
public:
  void enterPredicateBuild(PredicateBuild build);
  void markSummarizing(const FuncSymbol *function);
  void updatePredicateBuild(Flow whenTrue, Flow whenFalse);
  void rememberPredicate(const FuncSymbol *function, Predicate predicate);


  bool isBuildingPredicate() const;
  bool isSummarizing(const FuncSymbol *function) const;


  PredicateBuild leavePredicateBuild();
  const PredicateBuild &getPredicateBuild() const;
  const Predicate *findPredicate(const FuncSymbol *function) const;

private:
  std::vector<PredicateBuild> builds;
  llvm::SmallPtrSet<const FuncSymbol *, 8> summarizing;
  llvm::DenseMap<const FuncSymbol *, Predicate> predicates;
};

}
