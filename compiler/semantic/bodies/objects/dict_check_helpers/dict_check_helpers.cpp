#include "compiler/semantic/bodies/objects/dict_check_helpers/dict_check_helpers.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/process/process_member_table/process_member_table.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

std::string DictCheckHelpers::dictText(const Expr *expr, const Type *type) {
  const Expr *plain = ExprParts::withoutImplicitCasts(expr);
  auto *name = dyn_cast<NameExpr>(plain);
  if (name != nullptr) {
    return name->name.str();
  }
  auto *call = dyn_cast<CallExpr>(plain);
  if (call != nullptr && call->builtin() == Builtin::EnvMap) {
    auto process = GlobalNameTable::getName(GlobalName::PROCESS);
    auto env = ProcessMemberTable::getName(ProcessMember::ENV);
    return process.str() + "." + env.str();
  }
  auto *member = dyn_cast<MemberExpr>(plain);
  auto *object = member != nullptr ? dyn_cast<NameExpr>(member->object) : nullptr;
  if (object != nullptr) {
    return (object->name + "." + member->name).str();
  }

  return DictCheckHelpers::dictPlaceholder(type);
}

const char *DictCheckHelpers::dictPlaceholder(const Type *type) {
  if (type->isRecord()) {
    return "r";
  }
  if (type->isMap()) {
    return "m";
  }

  return "s";
}

}
