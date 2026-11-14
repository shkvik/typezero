#include "compiler/semantic/code/classes/overload_choice/choice_rules/choice_rules.h"

namespace tz {

ChoiceRules::ChoiceRules(SemanticState &owner)
    : state(owner) {}

VariantChoice ChoiceRules::failedChoice(const CodeClassesRegistryChooseVariantError &error) const {
  bool isReported = error.code == CodeClassesRegistryChooseVariantErrorCode::ALREADY_REPORTED;

  return VariantChoice{
      .function = nullptr,
      .isChecked = false,
      .typeArgs = {},
      .isReported = isReported,
      .message = error.message,
  };
}

const Type *ChoiceRules::parameterAt(const FuncSymbol *function, size_t index) const {
  bool isRest = function->hasRest && index + 1 >= function->params.size();
  if (isRest) {
    const Type *rest = function->params.back();

    return rest->isArray() ? rest->inner() : rest;
  }
  if (index >= function->params.size()) {
    return nullptr;
  }

  return function->params[index];
}

}
