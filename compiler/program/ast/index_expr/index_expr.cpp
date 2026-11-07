#include "compiler/program/ast/index_expr/index_expr.h"

namespace tz {

bool IndexExpr::mayBeOutOfRange() const {
  const ElementIndex *element = this->form.as<ElementIndex>();

  return element && element->mayBeOutOfRange;
}

bool IndexExpr::mayAppend() const {
  const ElementIndex *element = this->form.as<ElementIndex>();

  return element && element->mayAppend;
}

EnumDecl *IndexExpr::enumDecl() const {
  const EnumNameIndex *enumName = this->form.as<EnumNameIndex>();

  return enumName ? enumName->enumDecl : nullptr;
}

bool IndexExpr::keyed() const {
  return this->form.is<KeyedFieldIndex>();
}

llvm::StringRef IndexExpr::field() const {
  const NamedFieldIndex *named = this->form.as<NamedFieldIndex>();

  return named ? named->field : llvm::StringRef();
}

}
