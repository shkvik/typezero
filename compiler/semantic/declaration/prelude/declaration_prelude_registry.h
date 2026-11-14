#pragma once

#include "compiler/program/library/builtin_names/prelude_type_table/prelude_type_table.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class DeclarationPreludeRegistry {
public:
  explicit DeclarationPreludeRegistry(SemanticState &owner);

  void declarePrelude();

private:
  SemanticState &state;
};

}
