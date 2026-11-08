#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>
#include <set>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

bool GenericCheckHelpers::mentionsAnyOf(const Type *type, llvm::ArrayRef<const Type *> params,
                                        llvm::SmallPtrSetImpl<const Type *> &seen) {
  switch (type->kind()) {
  case Type::Kind::Param:
    if (type->typeOp() != TypeOp::None) {
      return llvm::any_of(type->operands(), [&](const Type *t) {
        return t && GenericCheckHelpers::mentionsAnyOf(t, params, seen);
      });
    }
    return llvm::is_contained(params, type);
  case Type::Kind::Optional:
  case Type::Kind::Array:
  case Type::Kind::Promise:
    return GenericCheckHelpers::mentionsAnyOf(type->inner(), params, seen);
  case Type::Kind::Result:
    return GenericCheckHelpers::mentionsAnyOf(type->okType(), params, seen) ||
           GenericCheckHelpers::mentionsAnyOf(type->errType(), params, seen);
  case Type::Kind::Function:
    return GenericCheckHelpers::mentionsAnyOf(type->ret(), params, seen) ||
           llvm::any_of(type->params(), [&](const Type *p) {
             return GenericCheckHelpers::mentionsAnyOf(p, params, seen);
           });
  case Type::Kind::Iterator:
    return GenericCheckHelpers::mentionsAnyOf(type->yielded(), params, seen) ||
           GenericCheckHelpers::mentionsAnyOf(type->returned(), params, seen) ||
           GenericCheckHelpers::mentionsAnyOf(type->sent(), params, seen);
  case Type::Kind::Dict:
    return GenericCheckHelpers::mentionsAnyOf(type->keyType(), params, seen) ||
           GenericCheckHelpers::mentionsAnyOf(type->valueType(), params, seen);
  case Type::Kind::ClassRef:
    return type->isConstructor() &&
           GenericCheckHelpers::mentionsAnyOf(type->constructed(), params, seen);
  case Type::Kind::Object:
    if (type->isAsyncIterator()) {
      return GenericCheckHelpers::mentionsAnyOf(type->yielded(), params, seen) ||
             GenericCheckHelpers::mentionsAnyOf(type->returned(), params, seen) ||
             GenericCheckHelpers::mentionsAnyOf(type->sent(), params, seen);
    }
    if (!type->typeArgs().empty()) {
      return llvm::any_of(type->typeArgs(), [&](const Type *t) {
        return GenericCheckHelpers::mentionsAnyOf(t, params, seen);
      });
    }
    if (!seen.insert(type).second) {
      return false;
    }
    if (type->tupleRest() && GenericCheckHelpers::mentionsAnyOf(type->tupleRest(), params, seen)) {
      return true;
    }
    return llvm::any_of(type->fields(), [&](const Field &f) {
      return GenericCheckHelpers::mentionsAnyOf(f.type, params, seen);
    });
  case Type::Kind::Union:
    return llvm::any_of(type->members(), [&](const Type *m) {
      return GenericCheckHelpers::mentionsAnyOf(m, params, seen);
    });
  default:
    return false;
  }
}

bool GenericCheckHelpers::mentionsAnyOf(const Type *type, llvm::ArrayRef<const Type *> params) {
  if (params.empty() || !TypeQueries::mentionsParam(type)) {
    return false;
  }
  llvm::SmallPtrSet<const Type *, 4> seen;

  return GenericCheckHelpers::mentionsAnyOf(type, params, seen);
}

std::string GenericCheckHelpers::constraintSpelling(const Type *param, const TypeParamDecl *decl) {
  if (decl && decl->constraint && decl->constraint->kind() == TypeRefKind::NAME_REF) {
    return decl->constraint->name.str();
  }
  if (decl && decl->constraint && decl->constraint->kind() == TypeRefKind::UNION_REF &&
      llvm::all_of(decl->constraint->args,
                   [](const TypeRef &t) { return t.kind() == TypeRefKind::NAME_REF; })) {
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

}
