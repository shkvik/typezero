#include "compiler/program/ast/class_field/class_field.h"

namespace tz {

bool ClassField::isStatic() const {
  return this->form.is<StaticField>();
}

bool ClassField::parameter() const {
  return this->form.is<ParameterField>();
}

FunctionDecl *ClassField::method() const {
  const ArrowMethodField *arrow = this->form.as<ArrowMethodField>();

  return arrow ? arrow->method : nullptr;
}

VarSymbol *ClassField::var() const {
  const StaticField *shared = this->form.as<StaticField>();

  return shared ? shared->var : nullptr;
}

bool ClassField::optional() const {
  return this->presence == FieldPresence::OPTIONAL;
}

bool ClassField::definite() const {
  return this->presence == FieldPresence::DEFINITE;
}

}
