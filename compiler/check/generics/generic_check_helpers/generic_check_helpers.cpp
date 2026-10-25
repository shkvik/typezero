#include "compiler/check/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/check/generics/inference/inference.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>
#include <set>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

bool mentionsAnyOf(const Type *type, llvm::ArrayRef<const Type *> params,
                   llvm::SmallPtrSetImpl<const Type *> &seen) {
  switch (type->kind()) {
  case Type::Kind::Param:
    // `T[K]` and the other computations, by what they are made of.
    if (type->typeOp() != TypeOp::None) {
      return llvm::any_of(type->operands(),
                          [&](const Type *t) { return t && mentionsAnyOf(t, params, seen); });
    }
    return llvm::is_contained(params, type);
  case Type::Kind::Optional:
  case Type::Kind::Array:
  case Type::Kind::Promise:
    return mentionsAnyOf(type->inner(), params, seen);
  case Type::Kind::Result:
    return mentionsAnyOf(type->okType(), params, seen) ||
           mentionsAnyOf(type->errType(), params, seen);
  case Type::Kind::Function:
    return mentionsAnyOf(type->ret(), params, seen) ||
           llvm::any_of(type->params(),
                        [&](const Type *p) { return mentionsAnyOf(p, params, seen); });
  case Type::Kind::Iterator:
    return mentionsAnyOf(type->yielded(), params, seen) ||
           mentionsAnyOf(type->returned(), params, seen) ||
           mentionsAnyOf(type->sent(), params, seen);
  case Type::Kind::Dict:
    return mentionsAnyOf(type->keyType(), params, seen) ||
           mentionsAnyOf(type->valueType(), params, seen);
  case Type::Kind::ClassRef: // `Constructor<T>`
    return type->isConstructor() && mentionsAnyOf(type->constructed(), params, seen);
  case Type::Kind::Object:
    // An async iterator, as an iterator.
    if (type->isAsyncIterator()) {
      return mentionsAnyOf(type->yielded(), params, seen) ||
             mentionsAnyOf(type->returned(), params, seen) ||
             mentionsAnyOf(type->sent(), params, seen);
    }
    // A class made from a generic one: by its type arguments.
    if (!type->typeArgs().empty()) {
      return llvm::any_of(type->typeArgs(),
                          [&](const Type *t) { return mentionsAnyOf(t, params, seen); });
    }
    if (!seen.insert(type).second) {
      return false;
    }
    if (type->tupleRest() && mentionsAnyOf(type->tupleRest(), params, seen)) {
      return true;
    }
    return llvm::any_of(type->fields(),
                        [&](const Field &f) { return mentionsAnyOf(f.type, params, seen); });
  case Type::Kind::Union:
    return llvm::any_of(type->members(),
                        [&](const Type *m) { return mentionsAnyOf(m, params, seen); });
  default:
    return false;
  }
}

std::string constraintSpelling(const Type *param, const TypeParamDecl *decl) {
  if (decl && decl->constraint && decl->constraint->kind == TypeRef::Kind::Name) {
    return decl->constraint->name.str();
  }
  if (decl && decl->constraint && decl->constraint->kind == TypeRef::Kind::Union &&
      llvm::all_of(decl->constraint->args,
                   [](const TypeRef &t) { return t.kind == TypeRef::Kind::Name; })) {
    std::string text;
    for (const TypeRef &t : decl->constraint->args) {
      text += (text.empty() ? "" : " | ") + t.name.str();
    }
    return text;
  }
  if (const Type *c = param->constraint()) {
    return c->name().str();
  }
  std::string text;
  for (const Type *t : param->alternatives()) {
    text += (text.empty() ? "" : " | ") + t->name().str();
  }

  return text;
}

} // namespace tz
