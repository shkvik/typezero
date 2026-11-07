#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ConditionFacts {
public:
  explicit ConditionFacts(Checker &owner);

  void addConditionFacts(const Expr *cond, Checker::Flow &whenTrue, Checker::Flow &whenFalse) const;

private:
  static const VarSymbol *variableOf(const Expr *expr);
  static const NameExpr *optionalVariable(const Expr *expr);
  static void addLess(Facts &facts, const Linear &x, const Linear &y, bool orEqual);

  bool chainDefined(const Expr *e, Checker::Flow &flow) const;

  Checker &checker;
};

}
