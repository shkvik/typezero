#pragma once

#include "compiler/semantic/code/conversions/code_conversions_registry_errors.h"
#include "compiler/semantic/code/conversions/conversion_rules/conversion_rules.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class ImplicitConversions {
public:
  ImplicitConversions(SemanticState &owner, ConversionRules &sharedConversionRules);

  void convert(const Convert &task);
  void convertSlot(const ConvertSlot &task);
  void apparentSlot(const ApparentSlot &task);
  void adaptLiteralSlot(const AdaptLiteralSlot &task);

private:
  SemanticState &state;
  ConversionRules &conversionRules;
};

}
