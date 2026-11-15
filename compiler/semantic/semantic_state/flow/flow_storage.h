#pragma once

#include "compiler/semantic/flow/changes/flow_changes.h"
#include "compiler/semantic/flow/conditions/flow_conditions.h"
#include "compiler/semantic/flow/defined_values/flow_defined_values.h"
#include "compiler/semantic/flow/function_facts/flow_function_facts.h"
#include "compiler/semantic/flow/inequalities/flow_inequalities.h"
#include "compiler/semantic/flow/joins/flow_joins.h"
#include "compiler/semantic/flow/loops/flow_loops.h"
#include "compiler/semantic/flow/measurements/flow_measurements.h"
#include "compiler/semantic/flow/narrowing/flow_narrowing.h"
#include "compiler/semantic/flow/proofs/flow_proofs.h"
#include "compiler/semantic/flow/reachability/flow_reachability.h"
#include "compiler/semantic/flow/tracked_values/flow_tracked_values.h"
#include "compiler/semantic/semantic_state/flow/assigned_field_context/assigned_field_context.h"
#include "compiler/semantic/semantic_state/flow/callback_context/callback_context.h"
#include "compiler/semantic/semantic_state/flow/fixed_length_context/fixed_length_context.h"
#include "compiler/semantic/semantic_state/flow/predicate_context/predicate_context.h"

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
