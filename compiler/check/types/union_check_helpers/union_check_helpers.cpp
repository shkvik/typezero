#include "compiler/check/types/union_check_helpers/union_check_helpers.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

const Type *writtenLiteral(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      return nullptr;
    }
    expr = conv->operand;
  }
  bool negated = false;
  if (auto *unary = dyn_cast<UnaryExpr>(expr); unary && unary->op == UnaryOp::Neg) {
    negated = true;
    expr = unary->operand;
  }
  if (auto *lit = dyn_cast<StringLitExpr>(expr)) {
    return negated ? nullptr : Type::stringLiteral(lit->value);
  }
  if (auto *lit = dyn_cast<BoolLitExpr>(expr)) {
    return negated ? nullptr : Type::boolLiteral(lit->value);
  }
  if (auto *lit = dyn_cast<IntLitExpr>(expr)) {
    double value = negated ? -double(lit->value) : double(lit->value);
    bool fits = std::fabs(value) <= 9007199254740992.0 && !(negated && lit->value == 0);
    return Type::numberLiteral(value, fits ? Type::intTy() : Type::doubleTy());
  }
  if (auto *lit = dyn_cast<FloatLitExpr>(expr)) {
    return Type::numberLiteral(negated ? -lit->value : lit->value, Type::doubleTy());
  }

  return nullptr;
}

}
