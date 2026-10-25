#include "compiler/check/objects/object_check_helpers/object_check_helpers.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

const Expr *unnarrowed(const Expr *expr) {
  auto *conv = dyn_cast<CastExpr>(expr);
  if (!conv || !conv->isImplicit() || conv->shortCircuit) {
    return expr;
  }
  const Type *from = conv->operand->type;
  if (from->isOptional() && conv->type == from->inner()) {
    return conv->operand;
  }
  if (isa<NameExpr>(conv->operand) && conv->type->isClass() && conv->type != from->nonOptional() &&
      conv->type->isSubtypeOf(from->nonOptional())) {
    return conv->operand;
  }

  return expr;
}

} // namespace tz
