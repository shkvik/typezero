#pragma once

#include "compiler/semantic/generics/apparent/generics_apparent.h"
#include "compiler/semantic/generics/constraints/generics_constraints.h"
#include "compiler/semantic/semantic_state/generics/alias_context/alias_context.h"
#include "compiler/semantic/semantic_state/generics/generic_copy_context/generic_copy_context.h"
#include "compiler/semantic/semantic_state/generics/type_parameter_context/type_parameter_context.h"

namespace tz {

struct GenericStorage {
  AliasContext aliases;
  GenericsApparent apparent;
  GenericCopyContext copies;
  TypeParameterContext parameters;
  GenericsConstraints constraints;
};

}
