#pragma once

#include "compiler/interprocedural/overflow/overflow_check/overflow_check.h"

namespace tz {

class Narrowing {
public:
  explicit Narrowing(OverflowCheck &check);

  void refine(const Expr *cond, State &st, State &whenTrue, State &whenFalse);
  void narrowComparison(const BinaryExpr *bin, const Val &l, const Val &r, State &whenTrue,
                        State &whenFalse);
  void applyPredicate(const CallExpr *call, State &whenTrue, State &whenFalse);
  void notePredicate(const FunctionDecl *fn, const State &whenTrue, const State &whenFalse);
  std::optional<OverflowCheck::Narrowable> narrowable(const Expr *expr, const State &st) const;
  void constrain(State &s, const OverflowCheck::Narrowable &target, const Bound &lower,
                 const Bound &upper);

private:
  using MaybeNarrowable = std::optional<OverflowCheck::Narrowable>;

  std::optional<double> literalValue(const Expr *expr) const;
  const Expr *stripNumericCasts(const Expr *expr) const;
  std::vector<const Expr *> narrowOrder(const Expr *expr) const;
  std::vector<const Expr *> narrowParts(const Expr *expr) const;
  MaybeNarrowable nodeNarrowable(const Expr *expr, const State &st,
                                 std::vector<MaybeNarrowable> &answers) const;
  MaybeNarrowable linearNarrowable(const BinaryExpr *bin, const MaybeNarrowable &left,
                                   const MaybeNarrowable &right) const;
  MaybeNarrowable absNarrowable(MaybeNarrowable inner) const;
  MaybeNarrowable leafNarrowable(const Expr *expr, const State &st) const;
  MaybeNarrowable takeNarrowable(std::vector<MaybeNarrowable> &answers) const;
  OverflowCheck &check;
};

}
