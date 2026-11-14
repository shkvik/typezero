#pragma once

#include "compiler/semantic/generics_v2/apparent/generics_apparent.h"
#include "compiler/semantic/generics_v2/constraints/generics_constraints.h"
#include "compiler/semantic/generics_v2/variance/generics_variance.h"
#include "compiler/semantic/semantic_state/v2/generics/alias_context/alias_context.h"
#include "compiler/semantic/semantic_state/v2/generics/generic_copy_context/generic_copy_context.h"
#include "compiler/semantic/semantic_state/v2/generics/type_parameter_context/type_parameter_context.h"

namespace tz {

struct GenericStorage {
  AliasContext aliases;
  GenericsVariance variance;
  GenericsApparent apparent;
  GenericCopyContext copies;
  TypeParameterContext parameters;
  GenericsConstraints constraints;
};

}
