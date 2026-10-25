#pragma once

#include "compiler/overflow/overflow_check/overflow_check.h"

namespace tz {

// What conditions say of the ranges: comparisons and predicates.
class Narrowing {
public:
  explicit Narrowing(OverflowCheck &check);

  void refine(const Expr *cond, State &st, State &whenTrue, State &whenFalse);
  void narrowComparison(const BinaryExpr *bin, const Val &l, const Val &r, State &whenTrue,
                        State &whenFalse);
  void applyPredicate(const CallExpr *call, State &whenTrue, State &whenFalse);
  void notePredicate(const FunctionDecl *fn, const State &whenTrue, const State &whenFalse);
  std::optional<OverflowCheck::Narrowable> narrowable(const Expr *expr, const State &st) const;
  void constrain(State &s, const OverflowCheck::Narrowable &target, const Bound &lower, const Bound &upper);

private:
  OverflowCheck &check_;
};

} // namespace tz
