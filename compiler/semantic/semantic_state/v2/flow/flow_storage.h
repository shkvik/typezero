#pragma once

#include "compiler/semantic/flow_v2/changes/flow_changes.h"
#include "compiler/semantic/flow_v2/conditions/flow_conditions.h"
#include "compiler/semantic/flow_v2/defined_values/flow_defined_values.h"
#include "compiler/semantic/flow_v2/function_facts/flow_function_facts.h"
#include "compiler/semantic/flow_v2/inequalities/flow_inequalities.h"
#include "compiler/semantic/flow_v2/joins/flow_joins.h"
#include "compiler/semantic/flow_v2/loops/flow_loops.h"
#include "compiler/semantic/flow_v2/measurements/flow_measurements.h"
#include "compiler/semantic/flow_v2/narrowing/flow_narrowing.h"
#include "compiler/semantic/flow_v2/proofs/flow_proofs.h"
#include "compiler/semantic/flow_v2/reachability/flow_reachability.h"
#include "compiler/semantic/flow_v2/tracked_values/flow_tracked_values.h"
#include "compiler/semantic/semantic_state/v2/flow/assigned_field_context/assigned_field_context.h"
#include "compiler/semantic/semantic_state/v2/flow/callback_context/callback_context.h"
#include "compiler/semantic/semantic_state/v2/flow/fixed_length_context/fixed_length_context.h"
#include "compiler/semantic/semantic_state/v2/flow/predicate_context/predicate_context.h"

namespace tz {

struct FlowStorage {
  FlowJoins joins;
  FlowLoops loops;
  FlowProofs proofs;
  FlowChanges changes;
  FlowNarrowing narrowing;
  CallbackContext callbacks;
  FlowConditions conditions;
  FlowDefinedValues defined;
  FlowTrackedValues tracked;
  FixedLengthContext lengths;
  FlowFunctionFacts functions;
  PredicateContext predicates;
  FlowInequalities inequalities;
  FlowMeasurements measurements;
  FlowReachability reachability;
  AssignedFieldContext assignments;
};

}
