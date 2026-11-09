#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/semantic/types/type_parts/type_routes.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>
#include <set>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

bool GenericCheckHelpers::mentionsAnyOf(const Type *type, llvm::ArrayRef<const Type *> params) {
  if (params.empty() || !TypeQueries::mentionsParam(type)) {
    return false;
  }
  TypeParts parts = TypeParts::collect(type, kMentionsRoute);
  for (const Type *part : parts.getTypes()) {
    if (GenericCheckHelpers::isParamOf(part, params)) {
      return true;
    }
  }

  return false;
}

bool GenericCheckHelpers::isParamOf(const Type *type, llvm::ArrayRef<const Type *> params) {
  bool plainParam = type->isParam() && type->typeOp() == TypeOp::None;

  return plainParam && llvm::is_contained(params, type);
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
