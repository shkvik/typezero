#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class ConditionFacts {
public:
  explicit ConditionFacts(SemanticState &owner);

  void addConditionFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const;

private:
  static const VarSymbol *variableOf(const Expr *expr);
  static const NameExpr *optionalVariable(const Expr *expr);
  static void addLess(Facts &facts, const Linear &x, const Linear &y, bool orEqual);

  bool chainDefined(const Expr *e, Flow &flow) const;

  SemanticState &state;
};

}
