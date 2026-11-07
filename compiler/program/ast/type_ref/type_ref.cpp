#include "compiler/program/ast/type_ref/type_ref.h"

namespace tz {

namespace {

const std::vector<llvm::StringRef> kNoNames;
const std::vector<llvm::SMLoc> kNoLocs;
const std::vector<bool> kNoFlags;
const std::u16string kNoText;
const std::vector<std::u16string> kNoTexts;

}

TypeRefKind TypeRef::kind() const {
  return this->form.kind();
}

const std::vector<llvm::StringRef> &TypeRef::names() const {
  const std::vector<llvm::StringRef> *found = &kNoNames;
  switch (this->form.kind()) {
    case TypeRefKind::FUNCTION_REF:
      found = &this->form.as<FunctionTypeRef>()->names;
      break;
    case TypeRefKind::OBJECT_REF:
      found = &this->form.as<ObjectTypeRef>()->names;
      break;
    case TypeRefKind::TYPE_OF_REF:
      found = &this->form.as<TypeOfTypeRef>()->names;
      break;
    case TypeRefKind::NAME_REF:
    case TypeRefKind::ARRAY_REF:
    case TypeRefKind::UNION_REF:
    case TypeRefKind::GENERIC_REF:
    case TypeRefKind::TUPLE_REF:
    case TypeRefKind::LITERAL_REF:
    case TypeRefKind::PREDICATE_REF:
    case TypeRefKind::KEY_OF_REF:
    case TypeRefKind::INDEXED_REF:
    case TypeRefKind::MAPPED_REF:
    case TypeRefKind::CONDITIONAL_REF:
    case TypeRefKind::INFER_REF:
    case TypeRefKind::TEMPLATE_REF:
    case TypeRefKind::INTERSECTION_REF:
      break;
  }

  return *found;
}

const std::vector<llvm::SMLoc> &TypeRef::locs() const {
  const ObjectTypeRef *object = this->form.as<ObjectTypeRef>();

  return object ? object->locs : kNoLocs;
}

const std::vector<bool> &TypeRef::optional() const {
  const std::vector<bool> *found = &kNoFlags;
  if (const FunctionTypeRef *function = this->form.as<FunctionTypeRef>()) {
    found = &function->optional;
  } else if (const ObjectTypeRef *object = this->form.as<ObjectTypeRef>()) {
    found = &object->optional;
  }

  return *found;
}

const std::vector<bool> &TypeRef::readonly() const {
  const std::vector<bool> *found = &kNoFlags;
  if (const ObjectTypeRef *object = this->form.as<ObjectTypeRef>()) {
    found = &object->readonly;
  } else if (const TupleTypeRef *tuple = this->form.as<TupleTypeRef>()) {
    found = &tuple->readonly;
  }

  return *found;
}

const std::vector<bool> &TypeRef::methods() const {
  const ObjectTypeRef *object = this->form.as<ObjectTypeRef>();

  return object ? object->methods : kNoFlags;
}

const std::vector<bool> &TypeRef::spread() const {
  const TupleTypeRef *tuple = this->form.as<TupleTypeRef>();

  return tuple ? tuple->spread : kNoFlags;
}

bool TypeRef::rest() const {
  const FunctionTypeRef *function = this->form.as<FunctionTypeRef>();

  return function && function->rest;
}

const std::u16string &TypeRef::text() const {
  const LiteralTypeRef *literal = this->form.as<LiteralTypeRef>();

  return literal ? literal->text : kNoText;
}

double TypeRef::number() const {
  const LiteralTypeRef *literal = this->form.as<LiteralTypeRef>();

  return literal ? literal->number : 0;
}

bool TypeRef::integer() const {
  const LiteralTypeRef *literal = this->form.as<LiteralTypeRef>();

  return literal && literal->integer;
}

const std::vector<std::u16string> &TypeRef::texts() const {
  const TemplateTypeRef *parts = this->form.as<TemplateTypeRef>();

  return parts ? parts->texts : kNoTexts;
}

bool TypeRef::asserts() const {
  const PredicateTypeRef *predicate = this->form.as<PredicateTypeRef>();

  return predicate && predicate->asserts;
}

int TypeRef::mappedOptional() const {
  const MappedTypeRef *mapped = this->form.as<MappedTypeRef>();

  return mapped ? mapped->mappedOptional : 0;
}

int TypeRef::mappedReadonly() const {
  const MappedTypeRef *mapped = this->form.as<MappedTypeRef>();

  return mapped ? mapped->mappedReadonly : 0;
}

}
