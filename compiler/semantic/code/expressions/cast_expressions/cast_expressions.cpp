#include "compiler/semantic/code/expressions/cast_expressions/cast_expressions.h"

using llvm::isa;

namespace tz {

CastExpressions::CastExpressions(SemanticState &owner)
    : state(owner) {}

void CastExpressions::afterCastTarget(const AfterCastTarget &task) {
  CastExpr *castExpr = task.castExpr;
  const Type *to = castExpr->target->resolved;
  bool isArrayLiteral = isa<ArrayLitExpr>(castExpr->operand);
  const Type *hint = isArrayLiteral && !to->isError() ? to : nullptr;
  this->state.walk.tasks.pushTask(
      FinishCast{.castExpr = castExpr, .isArrayLiteral = isArrayLiteral});

  return this->state.walk.tasks.pushTask(CheckExpr{.expr = castExpr->operand, .expected = hint});
}

void CastExpressions::finishCast(const FinishCast &task) {
  CastExpr *castExpr = task.castExpr;
  castExpr->operand = this->state.walk.tasks.takeChecked();
  castExpr->type = castExpr->target->resolved;
  auto supported = this->checkCast(castExpr, task.isArrayLiteral);
  if (supported.isErr()) {
    this->state.diagnostics.error(castExpr->loc(), supported.error().message);
  }

  return this->state.walk.tasks.pushChecked(castExpr);
}

CastExpressions::ResultCastSupported CastExpressions::checkCast(const CastExpr *castExpr,
                                                                bool isArrayLiteral) const {
  auto code = this->castProblem(castExpr->operand, castExpr->type, isArrayLiteral);
  auto message = this->castMessage(code, castExpr->operand, castExpr->type);

  return err(CodeExpressionsRegistryCheckCastError{
      .code = code,
      .message = message,
  });
}

CodeExpressionsRegistryCheckCastErrorCode
CastExpressions::castProblem(const Expr *operand, const Type *to, bool isArrayLiteral) const {
  const Type *from = operand->type;
  if (from->isError() || to->isError()) {
    return CodeExpressionsRegistryCheckCastErrorCode::NOT_SUPPORTED;
  }
  if (isArrayLiteral) {
    return CodeExpressionsRegistryCheckCastErrorCode::ARRAY_LITERAL;
  }
  if (from == to || this->state.types.relations.isImplicitlyConvertible(from, to)) {
    return CodeExpressionsRegistryCheckCastErrorCode::CONVERTS_BY_ITSELF;
  }
  if (from->isOptional() &&
      this->state.types.relations.isImplicitlyConvertible(from->inner(), to)) {
    return CodeExpressionsRegistryCheckCastErrorCode::MAY_BE_UNDEFINED;
  }
  auto hint = this->state.code.conversions.conversionHint(operand, to);
  if (!hint.empty()) {
    return CodeExpressionsRegistryCheckCastErrorCode::NEEDS_CONVERSION;
  }
  if (from->isFloat() && to->isDecimal()) {
    return CodeExpressionsRegistryCheckCastErrorCode::DOUBLE_TO_DECIMAL;
  }
  if (from->isDecimal() && to->isNumeric()) {
    return CodeExpressionsRegistryCheckCastErrorCode::DECIMAL_TO_NUMBER;
  }
  if (from->nonOptional()->isJson()) {
    return CodeExpressionsRegistryCheckCastErrorCode::JSON_VALUE;
  }

  return CodeExpressionsRegistryCheckCastErrorCode::CANNOT_BECOME;
}

std::string CastExpressions::castMessage(CodeExpressionsRegistryCheckCastErrorCode code,
                                         const Expr *operand, const Type *to) const {
  const Type *from = operand->type;
  switch (code) {
    case CodeExpressionsRegistryCheckCastErrorCode::NOT_SUPPORTED:
      return ErrorTextCast::plain();
    case CodeExpressionsRegistryCheckCastErrorCode::ARRAY_LITERAL:
      return ErrorTextCast::declare(to->name());
    case CodeExpressionsRegistryCheckCastErrorCode::CONVERTS_BY_ITSELF:
      return ErrorTextCast::itself(to->name());
    case CodeExpressionsRegistryCheckCastErrorCode::MAY_BE_UNDEFINED:
      return ErrorTextCast::undefined();
    case CodeExpressionsRegistryCheckCastErrorCode::NEEDS_CONVERSION:
      return this->hintedCastMessage(operand, to);
    case CodeExpressionsRegistryCheckCastErrorCode::DOUBLE_TO_DECIMAL:
      return ErrorTextCast::toDecimal();
    case CodeExpressionsRegistryCheckCastErrorCode::DECIMAL_TO_NUMBER:
      return ErrorTextCast::toNumber();
    case CodeExpressionsRegistryCheckCastErrorCode::JSON_VALUE:
      return ErrorTextCast::json();
    case CodeExpressionsRegistryCheckCastErrorCode::CANNOT_BECOME:
      return ErrorTextCast::cannot(from->name(), to->name());
  }

  return ErrorTextCast::cannot(from->name(), to->name());
}

std::string CastExpressions::hintedCastMessage(const Expr *operand, const Type *to) const {
  auto hint = this->state.code.conversions.conversionHint(operand, to);

  return ErrorTextCast::hinted(hint);
}

}
