#pragma once

#include "compiler/semantic/code/classes/code_classes_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "compiler/union/union/union.h"

namespace tz {

class ChoiceRules {
public:
  explicit ChoiceRules(SemanticState &owner);

  const Type *parameterAt(const FuncSymbol *function, size_t index) const;
  VariantChoice failedChoice(const CodeClassesRegistryChooseVariantError &error) const;

private:
  SemanticState &state;
};

}
