#pragma once

#include "compiler/semantic/semantic_state/v2/types/type_context/type_context.h"
#include "compiler/semantic/semantic_state/v2/types/widening_context/widening_context.h"
#include "compiler/semantic/types_v2/literals/types_literals.h"
#include "compiler/semantic/types_v2/relations/types_relations.h"

namespace tz {

struct TypeStorage {
  TypeContext context;
  WideningContext widenings;
  TypesLiterals literals{*this};
  TypesRelations relations{*this};
};

}
