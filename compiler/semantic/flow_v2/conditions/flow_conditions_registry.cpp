#include "compiler/semantic/flow_v2/conditions/flow_conditions_registry.h"

namespace tz {

FlowConditionsRegistry::FlowConditionsRegistry(SemanticState &owner)
    : state(owner) {}

void FlowConditionsRegistry::addConditionFacts(const AddConditionFacts &task) {
  const auto *function = this->state.flow.functions.findPredicateNeed(task.cond);
  if (function == nullptr) {
    return this->finishConditionFacts(FinishConditionFacts{.cond = task.cond});
  }
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(FinishConditionFacts{.cond = task.cond});

  return tasks.pushTask(BuildPredicate{.function = function});
}

void FlowConditionsRegistry::finishConditionFacts(const FinishConditionFacts &task) {
  auto flows = this->state.walk.tasks.takeConditionFlows();
  this->state.flow.conditions.addConditionFacts(task.cond, flows.whenTrue, flows.whenFalse);

  return this->state.walk.tasks.pushConditionFlows(std::move(flows));
}

}
