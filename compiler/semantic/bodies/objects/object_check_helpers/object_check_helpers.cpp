#include "compiler/semantic/bodies/objects/object_check_helpers/object_check_helpers.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

const Expr *ObjectCheckHelpers::unnarrowed(const Expr *expr) {
  auto *conversion = dyn_cast<CastExpr>(expr);
  if (conversion == nullptr || !conversion->isImplicit() || conversion->shortCircuit) {
    return expr;
  }
  const Type *from = conversion->operand->type;
  if (from->isOptional() && conversion->type == from->inner()) {
    return conversion->operand;
  }
  if (isa<NameExpr>(conversion->operand) && from->isResult() && conversion->type->isResult()) {
    return conversion->operand;
  }
  if (isa<NameExpr>(conversion->operand) && conversion->type->isClass() &&
      conversion->type != from->nonOptional() &&
      conversion->type->isSubtypeOf(from->nonOptional())) {
    return conversion->operand;
  }

  return expr;
}

}
