#include "compiler/semantic/bodies/objects/dict_check_helpers/dict_check_helpers.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

std::string DictCheckHelpers::dictText(const Expr *expr, const Type *type) {
  expr = ExprParts::withoutImplicitCasts(expr);
  auto *name = dyn_cast<NameExpr>(expr);
  if (name != nullptr) {
    return name->name.str();
  }
  auto *call = dyn_cast<CallExpr>(expr);
  if (call != nullptr && call->builtin() == Builtin::EnvMap) {
    return "process.env";
  }
  auto *member = dyn_cast<MemberExpr>(expr);
  if (member != nullptr) {
    auto *object = dyn_cast<NameExpr>(member->object);
    if (object != nullptr) {
      return (object->name + "." + member->name).str();
    }
  }

  return type->isRecord() ? "r" : type->isMap() ? "m" : "s";
}

}
