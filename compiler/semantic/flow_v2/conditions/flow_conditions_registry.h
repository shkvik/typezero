#pragma once

#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class FlowConditionsRegistry {
public:
  explicit FlowConditionsRegistry(SemanticState &owner);

  void addConditionFacts(const AddConditionFacts &task);
  void finishConditionFacts(const FinishConditionFacts &task);

private:
  SemanticState &state;
};

}
