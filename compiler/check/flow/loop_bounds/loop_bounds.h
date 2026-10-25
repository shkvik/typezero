#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The bounds of loop counters.
class LoopBounds {
public:
  explicit LoopBounds(Checker &checker);

  // The state at the head of a loop: what holds on entry and stays true
  // across iterations, given what the loop changes.
  Checker::Flow loopHead(const Checker::LoopScan &scan, const Expr *cond, bool induction) const;
  std::optional<int64_t> condBoundsAbove(const Expr *cond, const VarSymbol *var,
                                         const Checker::LoopScan &scan, int64_t step) const;
  bool condBoundsBelow(const Expr *cond, const VarSymbol *var, const Checker::LoopScan &scan,
                       int64_t step) const;
  bool boundedAbove(const Expr *expr, int64_t limit, const Checker::LoopScan &scan) const;
  bool boundedBelow(const Expr *expr, int64_t limit, const Checker::LoopScan &scan) const;
  // The range of an int expression judged by its form alone, before Checker
  // checks it: literals, int variables and lengths.
  std::optional<std::pair<int64_t, int64_t>> syntacticIntRange(const Expr *expr) const;

private:
  Checker &checker_;
};

} // namespace tz
