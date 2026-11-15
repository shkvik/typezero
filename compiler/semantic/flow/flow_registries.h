#pragma once

#include "compiler/semantic/flow/changes/flow_changes_registry.h"
#include "compiler/semantic/flow/conditions/flow_conditions_registry.h"
#include "compiler/semantic/flow/function_facts/flow_function_facts_registry.h"

namespace tz {

struct FlowRegistries {
  FlowChangesRegistry changes;
  FlowConditionsRegistry conditions;
  FlowFunctionFactsRegistry functions;
};

}
