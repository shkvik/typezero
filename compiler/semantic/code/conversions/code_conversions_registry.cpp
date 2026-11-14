#include "compiler/semantic/code/conversions/code_conversions_registry.h"

namespace tz {

CodeConversionsRegistry::CodeConversionsRegistry(SemanticState &owner)
    : conversionRules(owner),
      coercions(owner, this->conversionRules),
      implicitConversions(owner, this->conversionRules) {}

void CodeConversionsRegistry::convert(const Convert &task) {
  return this->implicitConversions.convert(task);
}

void CodeConversionsRegistry::convertSlot(const ConvertSlot &task) {
  return this->implicitConversions.convertSlot(task);
}

void CodeConversionsRegistry::adaptLiteralSlot(const AdaptLiteralSlot &task) {
  return this->implicitConversions.adaptLiteralSlot(task);
}

void CodeConversionsRegistry::apparentSlot(const ApparentSlot &task) {
  return this->implicitConversions.apparentSlot(task);
}

void CodeConversionsRegistry::coerce(const Coerce &task) {
  return this->coercions.coerce(task);
}

void CodeConversionsRegistry::coerceSlot(const CoerceSlot &task) {
  return this->coercions.coerceSlot(task);
}

void CodeConversionsRegistry::coerceIteratorFound(const CoerceIteratorFound &task) {
  return this->coercions.coerceIteratorFound(task);
}

void CodeConversionsRegistry::coerceAsyncIteratorFound(const CoerceAsyncIteratorFound &task) {
  return this->coercions.coerceAsyncIteratorFound(task);
}

}
