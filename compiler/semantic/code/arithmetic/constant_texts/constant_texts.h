#pragma once

#include "compiler/semantic/code/arithmetic/code_arithmetic_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>
#include <vector>

namespace tz {

class ConstantTexts {
public:
  explicit ConstantTexts(SemanticState &owner);

  void constText(const ConstText &task);

private:
  const Type *constTextType(const Expr *checked, const Type *expected);
  const Type *textLiteralFor(const Type *expected, const std::u16string &text);


  std::optional<std::u16string> writtenText(const Expr *expr) const;
  std::optional<std::vector<std::u16string>> choiceTexts(const Expr *expr) const;

  SemanticState &state;
};

}
