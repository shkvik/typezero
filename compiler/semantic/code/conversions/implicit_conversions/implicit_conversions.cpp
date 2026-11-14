#include "compiler/semantic/code/conversions/implicit_conversions/implicit_conversions.h"
#include <cctype>

namespace tz {

ImplicitConversions::ImplicitConversions(SemanticState &owner,
                                         ConversionRules &sharedConversionRules)
    : state(owner),
      conversionRules(sharedConversionRules) {}

void ImplicitConversions::convert(const Convert &task) {
  Expr *checked = this->state.walk.tasks.takeChecked();
  auto *converted = this->conversionRules.convertExpr(checked, task.to);

  return this->state.walk.tasks.pushChecked(converted);
}

void ImplicitConversions::convertSlot(const ConvertSlot &task) {
  *task.slot = this->conversionRules.convertExpr(*task.slot, task.to);
}

void ImplicitConversions::adaptLiteralSlot(const AdaptLiteralSlot &task) {
  *task.slot = this->conversionRules.adaptLiteralExpr(*task.slot, task.to);
}

void ImplicitConversions::apparentSlot(const ApparentSlot &task) {
  Expr *expr = *task.slot;
  const Type *apparent = this->state.generics.apparent.apparentType(expr->type);
  if (apparent != expr->type) {
    *task.slot = this->conversionRules.convertExpr(expr, apparent);
  }
}

}
