#include "compiler/semantic/flow_v2/narrowing/flow_narrowing.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

FlowNarrowing::FlowNarrowing(SemanticState &owner)
    : unionFacts(owner),
      unknownFacts(owner),
      variableFacts(owner) {}

void FlowNarrowing::addVariableInstanceOfFacts(const BinaryExpr *binary, Flow &whenTrue) const {
  return this->variableFacts.addVariableInstanceOfFacts(binary, whenTrue);
}

void FlowNarrowing::addVariableTypeOfFacts(const BinaryExpr *binary, Flow &whenTrue,
                                           Flow &whenFalse) const {
  return this->variableFacts.addVariableTypeOfFacts(binary, whenTrue, whenFalse);
}

void FlowNarrowing::narrowVariable(Flow &flow, const VarSymbol *variable, const Type *type) const {
  return this->unionFacts.narrowVariable(flow, variable, type);
}

void FlowNarrowing::addUnionFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const {
  return this->unionFacts.addUnionFacts(cond, whenTrue, whenFalse);
}

void FlowNarrowing::addUnknownFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const {
  return this->unknownFacts.addUnknownFacts(cond, whenTrue, whenFalse);
}

}
