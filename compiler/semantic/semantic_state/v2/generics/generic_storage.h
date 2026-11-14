#pragma once

#include "compiler/semantic/semantic_state/v2/generics/alias_context/alias_context.h"
#include "compiler/semantic/semantic_state/v2/generics/generic_copy_context/generic_copy_context.h"
#include "compiler/semantic/semantic_state/v2/generics/type_parameter_context/type_parameter_context.h"

namespace tz {

struct GenericStorage {
  AliasContext aliases;
  GenericCopyContext copies;
  TypeParameterContext parameters;
};

}
