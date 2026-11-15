#pragma once

#include "compiler/semantic/generics/variance/generics_variance.h"
#include "compiler/semantic/semantic_state/types/type_context/type_context.h"
#include "compiler/semantic/semantic_state/types/widening_context/widening_context.h"
#include "compiler/semantic/types/literals/types_literals.h"
#include "compiler/semantic/types/references/types_references.h"
#include "compiler/semantic/types/relations/types_relations.h"

namespace tz {

struct TypeStorage {
  TypeContext context;
  WideningContext widenings;
  TypesLiterals literals{*this};
  GenericsVariance variance{*this};
  TypesReferences references{*this};
  TypesRelations relations{*this};
};

}
