#pragma once

#include "compiler/semantic/code/conversions/coercions/coercions.h"
#include "compiler/semantic/code/conversions/conversion_rules/conversion_rules.h"
#include "compiler/semantic/code/conversions/implicit_conversions/implicit_conversions.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class CodeConversionsRegistry {
public:
  explicit CodeConversionsRegistry(SemanticState &owner);

  void coerce(const Coerce &task);
  void convert(const Convert &task);
  void coerceSlot(const CoerceSlot &task);
  void convertSlot(const ConvertSlot &task);
  void apparentSlot(const ApparentSlot &task);
  void adaptLiteralSlot(const AdaptLiteralSlot &task);
  void coerceIteratorFound(const CoerceIteratorFound &task);
  void coerceAsyncIteratorFound(const CoerceAsyncIteratorFound &task);

private:
  ConversionRules conversionRules;
  Coercions coercions;
  ImplicitConversions implicitConversions;
};

}
