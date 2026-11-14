#pragma once

#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class GenericsConstraintsRegistry {
public:
  explicit GenericsConstraintsRegistry(SemanticState &owner);

  void checkConstraint(const CheckConstraint &task);
  void evaluateConstraint(const EvaluateConstraint &task);
  void afterConstraintMethod(const AfterConstraintMethod &task);
  void afterConstraintSubstitution(const AfterConstraintSubstitution &task);

private:
  SemanticState &state;
};

}
