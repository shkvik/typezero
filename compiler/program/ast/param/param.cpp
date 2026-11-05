#include "compiler/program/ast/param/param.h"

namespace tz {

bool Param::optional() const {
  return this->form.is<OptionalParam>();
}

bool Param::rest() const {
  return this->form.is<RestParam>();
}

Expr *Param::defaultValue() const {
  const DefaultedParam *defaulted = this->form.as<DefaultedParam>();

  return defaulted ? defaulted->defaultValue : nullptr;
}

}
