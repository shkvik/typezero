#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The bounds of loop counters.
class LoopBounds {
public:
  explicit LoopBounds(Sema &sema);

  // The state at the head of a loop: what holds on entry and stays true
  // across iterations, given what the loop changes.
  Sema::Flow loopHead(const Sema::LoopScan &scan, const Expr *cond, bool induction) const;
  std::optional<int64_t> condBoundsAbove(const Expr *cond, const VarSymbol *var,
                                         const Sema::LoopScan &scan, int64_t step) const;
  bool condBoundsBelow(const Expr *cond, const VarSymbol *var, const Sema::LoopScan &scan,
                       int64_t step) const;
  bool boundedAbove(const Expr *expr, int64_t limit, const Sema::LoopScan &scan) const;
  bool boundedBelow(const Expr *expr, int64_t limit, const Sema::LoopScan &scan) const;
  // The range of an int expression judged by its form alone, before Sema
  // checks it: literals, int variables and lengths.
  std::optional<std::pair<int64_t, int64_t>> syntacticIntRange(const Expr *expr) const;

private:
  Sema &sema_;
};

} // namespace tz
