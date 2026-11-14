#pragma once

#include "compiler/semantic/semantic_state/v2/types/type_context/type_context.h"
#include "compiler/semantic/semantic_state/v2/types/widening_context/widening_context.h"

namespace tz {

struct TypeStorage {
  TypeContext context;
  WideningContext widenings;
};

}
