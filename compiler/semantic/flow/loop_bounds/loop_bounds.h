#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class LoopBounds {
public:
  explicit LoopBounds(SemanticState &owner);

  Flow loopHead(const LoopScan &scan, const Expr *cond, bool induction) const;
  std::optional<int64_t> condBoundsAbove(const Expr *cond, const VarSymbol *var,
                                         const LoopScan &scan, int64_t step) const;
  bool condBoundsBelow(const Expr *cond, const VarSymbol *var, const LoopScan &scan,
                       int64_t step) const;
  bool boundedAbove(const Expr *expr, int64_t limit, const LoopScan &scan) const;
  bool boundedBelow(const Expr *expr, int64_t limit, const LoopScan &scan) const;
  std::optional<std::pair<int64_t, int64_t>> syntacticIntRange(const Expr *expr) const;

private:
  static std::optional<int64_t>
  varPlus(const Expr *expr, const VarSymbol *var,
          llvm::function_ref<const VarSymbol *(llvm::StringRef)> lookupVar);
  static bool isArrayLength(const Term &t);

  SemanticState &state;
};

}
