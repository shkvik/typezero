#include "compiler/program/ast/object_lit_expr/object_lit_expr.h"

namespace tz {

bool ObjectLitExpr::Property::spread() const {
  return this->form.is<SpreadProperty>();
}

bool ObjectLitExpr::Property::defaulted() const {
  return this->form.is<DefaultedProperty>();
}

bool ObjectLitExpr::Property::method() const {
  return this->form.is<MethodProperty>();
}

bool ObjectLitExpr::Property::keyed() const {
  const ComputedProperty *computed = this->form.as<ComputedProperty>();

  return computed && computed->keyed;
}

Expr *ObjectLitExpr::Property::key() const {
  const ComputedProperty *computed = this->form.as<ComputedProperty>();

  return computed ? computed->key : nullptr;
}

llvm::ArrayRef<llvm::StringRef> ObjectLitExpr::Property::omit() const {
  const SpreadProperty *copied = this->form.as<SpreadProperty>();

  return copied ? llvm::ArrayRef<llvm::StringRef>(copied->omit) : llvm::ArrayRef<llvm::StringRef>();
}

llvm::ArrayRef<const IndexExpr *> ObjectLitExpr::Property::omitKeys() const {
  const SpreadProperty *copied = this->form.as<SpreadProperty>();

  return copied ? llvm::ArrayRef<const IndexExpr *>(copied->omitKeys)
                : llvm::ArrayRef<const IndexExpr *>();
}

llvm::ArrayRef<llvm::StringRef> ObjectLitExpr::Property::replacedFields() const {
  const SpreadProperty *copied = this->form.as<SpreadProperty>();

  return copied ? llvm::ArrayRef<llvm::StringRef>(copied->replacedFields)
                : llvm::ArrayRef<llvm::StringRef>();
}

bool ObjectLitExpr::hasSpread() const {
  return llvm::any_of(this->props, [](const Property &p) { return p.spread(); });
}

}
