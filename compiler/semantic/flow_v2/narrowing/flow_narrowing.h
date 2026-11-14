#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/flow_v2/narrowing/union_facts/union_facts.h"
#include "compiler/semantic/flow_v2/narrowing/unknown_facts/unknown_facts.h"
#include "compiler/semantic/flow_v2/narrowing/variable_facts/variable_facts.h"
#include "compiler/semantic/semantic_state/flow_state/flow_state_types.h"

namespace tz {

class SemanticState;

class FlowNarrowing {
public:
  explicit FlowNarrowing(SemanticState &owner);

  void addUnionFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const;
  void addUnknownFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const;
  void addVariableInstanceOfFacts(const BinaryExpr *binary, Flow &whenTrue) const;
  void narrowVariable(Flow &flow, const VarSymbol *variable, const Type *type) const;
  void addVariableTypeOfFacts(const BinaryExpr *binary, Flow &whenTrue, Flow &whenFalse) const;

private:
  UnionFacts unionFacts;
  UnknownFacts unknownFacts;
  VariableFacts variableFacts;
};

}
