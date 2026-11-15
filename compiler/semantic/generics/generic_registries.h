#pragma once

#include "compiler/semantic/generics/aliases/generics_aliases_registry.h"
#include "compiler/semantic/generics/classes/generics_classes_registry.h"
#include "compiler/semantic/generics/constraints/generics_constraints_registry.h"
#include "compiler/semantic/generics/copies/generics_copies_registry.h"
#include "compiler/semantic/generics/functions/generics_functions_registry.h"
#include "compiler/semantic/generics/inference/generics_inference_registry.h"
#include "compiler/semantic/generics/parameters/generics_parameters_registry.h"
#include "compiler/semantic/generics/substitution/generics_substitution_registry.h"
#include "compiler/semantic/generics/variance/generics_variance_registry.h"

namespace tz {

struct GenericRegistries {
  GenericsCopiesRegistry copies;
  GenericsAliasesRegistry aliases;
  GenericsClassesRegistry classes;
  GenericsVarianceRegistry variance;
  GenericsFunctionsRegistry functions;
  GenericsInferenceRegistry inference;
  GenericsParametersRegistry parameters;
  GenericsConstraintsRegistry constraints;
  GenericsSubstitutionRegistry substitution;
};

}
