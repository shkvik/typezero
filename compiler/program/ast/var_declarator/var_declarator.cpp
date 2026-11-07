#include "compiler/program/ast/var_declarator/var_declarator.h"

namespace tz {

bool VarDeclarator::destructured() const {
  return !this->form.is<PlainBinding>();
}

bool VarDeclarator::objectPattern() const {
  return this->form.is<ObjectPattern>();
}

size_t VarDeclarator::patternLength() const {
  const ArrayPattern *array = this->form.as<ArrayPattern>();

  return array ? array->length : 0;
}

bool VarDeclarator::patternRest() const {
  const ArrayPattern *array = this->form.as<ArrayPattern>();

  return array && array->rest;
}

}
