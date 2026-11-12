#include "compiler/semantic/types/union_check_helpers/union_check_helpers.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>

using llvm::dyn_cast;

namespace tz {

namespace {

constexpr double kMaxWholeNumber = 9007199254740992.0;

}

const Type *UnionCheckHelpers::writtenLiteral(const Expr *expr) {
  const Expr *bare = ExprParts::withoutImplicitCasts(expr);
  auto *unary = dyn_cast<UnaryExpr>(bare);
  bool isNegated = unary != nullptr && unary->op == UnaryOp::Neg;
  const Expr *value = isNegated ? unary->operand : bare;
  auto *text = dyn_cast<StringLitExpr>(value);
  auto *flag = dyn_cast<BoolLitExpr>(value);
  auto *whole = dyn_cast<IntLitExpr>(value);
  auto *fraction = dyn_cast<FloatLitExpr>(value);

  if (text != nullptr && !isNegated) {
    return Type::stringLiteral(text->value);
  }
  if (flag != nullptr && !isNegated) {
    return Type::boolLiteral(flag->value);
  }
  if (whole != nullptr) {
    return UnionCheckHelpers::wholeLiteral(whole, isNegated);
  }
  if (fraction != nullptr) {
    return UnionCheckHelpers::fractionLiteral(fraction, isNegated);
  }

  return nullptr;
}

const Type *UnionCheckHelpers::wholeLiteral(const IntLitExpr *whole, bool isNegated) {
  double value = isNegated ? -double(whole->value) : double(whole->value);
  bool isNegativeZero = isNegated && whole->value == 0;
  bool isFitting = std::fabs(value) <= kMaxWholeNumber && !isNegativeZero;
  if (isFitting) {
    return Type::numberLiteral(value, Type::intTy());
  }

  return Type::numberLiteral(value, Type::doubleTy());
}

const Type *UnionCheckHelpers::fractionLiteral(const FloatLitExpr *fraction, bool isNegated) {
  double value = isNegated ? -fraction->value : fraction->value;

  return Type::numberLiteral(value, Type::doubleTy());
}

}
