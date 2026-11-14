#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/semantic_state/flow_state/flow_state_types.h"

namespace tz {

class SemanticState;

class FlowConditions {
public:
  explicit FlowConditions(SemanticState &owner);

  void addConditionFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const;

private:
  void addOwnFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const;
  void addBinaryFacts(const BinaryExpr *binary, Flow &whenTrue, Flow &whenFalse) const;

  SemanticState &state;
};

}
