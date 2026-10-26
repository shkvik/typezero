#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class UnionNarrowing {
public:
  explicit UnionNarrowing(Checker &checker);

  static void narrowVariable(Checker::Flow &flow, const VarSymbol *var, const Type *type);
  std::optional<Checker::NarrowTarget> narrowTarget(const Expr *expr) const;
  const Type *targetType(const Checker::Flow &flow, const Checker::NarrowTarget &target) const;
  void narrowTargetTo(Checker::Flow &flow, const Checker::NarrowTarget &target,
                      const Type *type) const;
  void addUnionFacts(const Expr *cond, Checker::Flow &whenTrue, Checker::Flow &whenFalse) const;

private:
  Checker &checker_;
};

}
