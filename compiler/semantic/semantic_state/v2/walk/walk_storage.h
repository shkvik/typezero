#pragma once

#include "compiler/semantic/semantic_state/v2/walk/block_position_context/block_position_context.h"
#include "compiler/semantic/semantic_state/v2/walk/class_context/class_context.h"
#include "compiler/semantic/semantic_state/v2/walk/flow_state_context/flow_state_context.h"
#include "compiler/semantic/semantic_state/v2/walk/function_stack_context/function_stack_context.h"
#include "compiler/semantic/semantic_state/v2/walk/scope_stack_context/scope_stack_context.h"

namespace tz {

struct WalkStorage {
  ClassContext self;
  FlowStateContext flow;
  ScopeStackContext scopes;
  BlockPositionContext block;
  FunctionStackContext functions;
};

}
