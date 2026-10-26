#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class LoopBounds {
public:
  explicit LoopBounds(Checker &checker);

  Checker::Flow loopHead(const Checker::LoopScan &scan, const Expr *cond, bool induction) const;
  std::optional<int64_t> condBoundsAbove(const Expr *cond, const VarSymbol *var,
                                         const Checker::LoopScan &scan, int64_t step) const;
  bool condBoundsBelow(const Expr *cond, const VarSymbol *var, const Checker::LoopScan &scan,
                       int64_t step) const;
  bool boundedAbove(const Expr *expr, int64_t limit, const Checker::LoopScan &scan) const;
  bool boundedBelow(const Expr *expr, int64_t limit, const Checker::LoopScan &scan) const;
  std::optional<std::pair<int64_t, int64_t>> syntacticIntRange(const Expr *expr) const;

private:
  Checker &checker_;
};

}
