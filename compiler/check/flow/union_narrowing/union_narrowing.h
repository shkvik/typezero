#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Narrowing of unions by tests.
class UnionNarrowing {
public:
  explicit UnionNarrowing(Checker &checker);

  // Narrows `var` to `type` in `flow`; null for no type left (never).
  static void narrowVariable(Checker::Flow &flow, const VarSymbol *var, const Type *type);
  std::optional<Checker::NarrowTarget> narrowTarget(const Expr *expr) const;
  const Type *targetType(const Checker::Flow &flow, const Checker::NarrowTarget &target) const;
  void narrowTargetTo(Checker::Flow &flow, const Checker::NarrowTarget &target,
                      const Type *type) const;
  // What a check says about a variable or a field of a union: its tag,
  // `x == "up"`, `typeof`, `in`, `instanceof`, `Array.isArray`.
  void addUnionFacts(const Expr *cond, Checker::Flow &whenTrue, Checker::Flow &whenFalse) const;

private:
  Checker &checker_;
};

} // namespace tz
