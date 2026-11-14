#include "compiler/semantic/flow_v2/measurements/flow_measurements.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

FlowMeasurements::FlowMeasurements(SemanticState &owner)
    : builtinLengths(owner),
      valueRanges(owner),
      linearForms(owner, this->builtinLengths) {}

const VarSymbol *FlowMeasurements::variableOf(const Expr *expr) const {
  return this->linearForms.variableOf(expr);
}

std::optional<Linear> FlowMeasurements::linear(const Expr *expr) const {
  return this->linearForms.linear(expr);
}

std::optional<Linear> FlowMeasurements::lengthOf(const Expr *expr) const {
  return this->linearForms.lengthOf(expr);
}

bool FlowMeasurements::isRawBuffer(const Type *type) const {
  return this->linearForms.isRawBuffer(type);
}

std::optional<int64_t> FlowMeasurements::exactLength(const Expr *object) const {
  return this->linearForms.exactLength(object);
}

std::optional<Linear> FlowMeasurements::lengthAtMost(const Expr *expr) const {
  return this->linearForms.lengthAtMost(expr);
}

Linear FlowMeasurements::settled(Linear value) const {
  return this->linearForms.settled(value);
}

IntegerRange FlowMeasurements::lengthRange(const Expr *value) const {
  return this->linearForms.lengthRange(value);
}

std::optional<Linear> FlowMeasurements::knownLength(const Expr *value) const {
  return this->linearForms.knownLength(value);
}

IntegerRange FlowMeasurements::valueRange(const Expr *expr) const {
  return this->valueRanges.valueRange(expr);
}

}
