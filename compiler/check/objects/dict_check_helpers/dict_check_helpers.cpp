#include "compiler/check/objects/dict_check_helpers/dict_check_helpers.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

std::string dictText(const Expr *expr, const Type *type) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      break;
    }
    expr = conv->operand;
  }
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    return name->name.str();
  }
  if (auto *call = dyn_cast<CallExpr>(expr); call && call->builtin == Builtin::EnvMap) {
    return "process.env";
  }
  if (auto *member = dyn_cast<MemberExpr>(expr)) {
    if (auto *object = dyn_cast<NameExpr>(member->object)) {
      return (object->name + "." + member->name).str();
    }
  }

  return type->isRecord() ? "r" : type->isMap() ? "m" : "s";
}

} // namespace tz
