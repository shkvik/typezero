#pragma once

#include "compiler/semantic/semantic_state/walk/block_position_context/block_position_context.h"
#include "compiler/semantic/semantic_state/walk/flow_state_context/flow_state_types.h"
#include "compiler/semantic/semantic_state/walk/function_stack_context/function_stack_types.h"
#include "compiler/semantic/semantic_state/walk/scope_stack_context/scope_stack_context.h"

namespace tz {

struct Module;
struct InstanceRecord;

struct Checkpoint {
  Flow flow;
  BlockSnapshot block;
  ScopeSnapshot scopes;
  Module *module = nullptr;
  FunctionSnapshot functions;
  InstanceRecord *instance = nullptr;
};

}
