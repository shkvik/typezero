#include "compiler/semantic/types/union_check_helpers/union_check_helpers.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

const Type *UnionCheckHelpers::writtenLiteral(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      return nullptr;
    }
    expr = conv->operand;
  }
  bool isNegated = false;
  if (auto *unary = dyn_cast<UnaryExpr>(expr); unary && unary->op == UnaryOp::Neg) {
    isNegated = true;
    expr = unary->operand;
  }
  if (auto *lit = dyn_cast<StringLitExpr>(expr)) {
    return isNegated ? nullptr : Type::stringLiteral(lit->value);
  }
  if (auto *lit = dyn_cast<BoolLitExpr>(expr)) {
    return isNegated ? nullptr : Type::boolLiteral(lit->value);
  }
  if (auto *lit = dyn_cast<IntLitExpr>(expr)) {
    double value = isNegated ? -double(lit->value) : double(lit->value);
    bool isFitting = std::fabs(value) <= 9007199254740992.0 && !(isNegated && lit->value == 0);
    return Type::numberLiteral(value, isFitting ? Type::intTy() : Type::doubleTy());
  }
  if (auto *lit = dyn_cast<FloatLitExpr>(expr)) {
    return Type::numberLiteral(isNegated ? -lit->value : lit->value, Type::doubleTy());
  }

  return nullptr;
}

}
