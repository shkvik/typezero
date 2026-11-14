#pragma once

#include "compiler/semantic/semantic_state/v2/flow/assigned_field_context/assigned_field_context.h"
#include "compiler/semantic/semantic_state/v2/flow/callback_context/callback_context.h"
#include "compiler/semantic/semantic_state/v2/flow/fixed_length_context/fixed_length_context.h"
#include "compiler/semantic/semantic_state/v2/flow/predicate_context/predicate_context.h"

namespace tz {

struct FlowStorage {
  CallbackContext callbacks;
  FixedLengthContext lengths;
  PredicateContext predicates;
  AssignedFieldContext assignments;
};

}
