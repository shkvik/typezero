#include "compiler/program/ast/new_expr/new_expr.h"

namespace tz {

ClassDecl *NewExpr::cls() const {
  const ClassNew *made = this->form.as<ClassNew>();

  return made ? made->cls : nullptr;
}

CallExpr *NewExpr::ctor() const {
  const ClassNew *made = this->form.as<ClassNew>();

  return made ? made->ctor : nullptr;
}

bool NewExpr::reflect() const {
  return this->form.is<ReflectNew>();
}

}
