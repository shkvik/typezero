#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Narrowing of unions by tests.
class UnionNarrowing {
public:
  explicit UnionNarrowing(Sema &sema);

  // Narrows `var` to `type` in `flow`; null for no type left (never).
  static void narrowVariable(Sema::Flow &flow, const VarSymbol *var, const Type *type);
  std::optional<Sema::NarrowTarget> narrowTarget(const Expr *expr) const;
  const Type *targetType(const Sema::Flow &flow, const Sema::NarrowTarget &target) const;
  void narrowTargetTo(Sema::Flow &flow, const Sema::NarrowTarget &target, const Type *type) const;
  // What a check says about a variable or a field of a union: its tag,
  // `x == "up"`, `typeof`, `in`, `instanceof`, `Array.isArray`.
  void addUnionFacts(const Expr *cond, Sema::Flow &whenTrue, Sema::Flow &whenFalse) const;

private:
  Sema &sema_;
};

} // namespace tz
