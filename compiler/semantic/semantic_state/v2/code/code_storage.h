#pragma once

#include "compiler/semantic/semantic_state/v2/code/argument_list_context/argument_list_context.h"
#include "compiler/semantic/semantic_state/v2/code/concat_chain_context/concat_chain_context.h"
#include "compiler/semantic/semantic_state/v2/code/constant_context/constant_context.h"
#include "compiler/semantic/semantic_state/v2/code/constructor_context/constructor_context.h"
#include "compiler/semantic/semantic_state/v2/code/decorator_context/decorator_context.h"
#include "compiler/semantic/semantic_state/v2/code/late_class_context/late_class_context.h"
#include "compiler/semantic/semantic_state/v2/code/literal_context/literal_context.h"
#include "compiler/semantic/semantic_state/v2/code/overload_context/overload_context.h"
#include "compiler/semantic/semantic_state/v2/code/variable_context/variable_context.h"

namespace tz {

struct CodeStorage {
  LateClassContext late;
  LiteralContext literals;
  ConstantContext constants;
  OverloadContext overloads;
  VariableContext variables;
  DecoratorContext decorators;
  ArgumentListContext arguments;
  ConstructorContext constructors;
  ConcatChainContext concatenation;
};

}
