#include "compiler/semantic/code/logic/nullish_values/nullish_values.h"
#include "compiler/semantic/code/operators/code_operators_registry_errors.h"

using llvm::isa;

namespace tz {

NullishValues::NullishValues(SemanticState &owner)
    : state(owner) {}

void NullishValues::startNullish(const StartNullish &task) {
  this->state.walk.tasks.pushTask(
      AfterNullishLeft{.binary = task.binary, .expected = task.expected});

  return this->state.walk.tasks.pushTask(
      CheckExpr{.expr = task.binary->lhs, .expected = task.expected});
}

void NullishValues::afterNullishLeft(const AfterNullishLeft &task) {
  Expr *left = this->state.walk.tasks.takeChecked();
  bool isMatchItems = left->type->nonOptional()->isMatch() && isa<ArrayLitExpr>(task.binary->rhs);
  this->state.walk.tasks.pushTask(
      StartNullishRight{.binary = task.binary, .expected = task.expected});
  this->state.walk.tasks.pushChecked(left);
  if (isMatchItems) {
    this->state.walk.tasks.pushTask(MatchItems{});
  }
}

void NullishValues::startNullishRight(const StartNullishRight &task) {
  BinaryExpr *binary = task.binary;
  binary->lhs = this->state.walk.tasks.takeChecked();
  const Type *leftType = binary->lhs->type;
  auto afterLeft = this->state.walk.flow.getFlow();
  const Type *hint =
      leftType->isUndefined() || leftType->isError() ? task.expected : leftType->nonOptional();
  this->state.walk.tasks.pushTask(
      FinishNullish{.binary = binary, .afterLeft = std::move(afterLeft)});

  return this->state.walk.tasks.pushTask(CheckExpr{.expr = binary->rhs, .expected = hint});
}

void NullishValues::finishNullish(const FinishNullish &task) {
  BinaryExpr *binary = task.binary;
  binary->rhs = this->state.walk.tasks.takeChecked();
  const auto &afterRight = this->state.walk.flow.getFlow();
  auto joined = this->state.flow.joins.join(task.afterLeft, afterRight);
  this->state.walk.flow.replaceFlow(std::move(joined));
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  binary->type = Type::error();
  if (leftType->isError() || rightType->isError()) {
    binary->type = this->failedNullishType(leftType, rightType);
    return this->state.walk.tasks.pushChecked(binary);
  }
  auto resolved = this->checkNullishType(leftType, rightType);
  if (resolved.isErr()) {
    this->state.diagnostics.error(binary->loc(), resolved.error().message);
    return this->state.walk.tasks.pushChecked(binary);
  }
  const Type *result = resolved.value();
  binary->type = result;
  this->state.walk.tasks.pushChecked(binary);
  if (leftType->isUndefined()) {
    return;
  }
  const Type *leftTarget = leftType->isOptional() ? Type::optional(result->nonOptional()) : result;
  this->state.walk.tasks.pushTask(ConvertSlot{.slot = &binary->rhs, .to = result});

  return this->state.walk.tasks.pushTask(ConvertSlot{.slot = &binary->lhs, .to = leftTarget});
}

NullishValues::ResultNullishResolved NullishValues::checkNullishType(const Type *leftType,
                                                                     const Type *rightType) const {
  if (leftType->isVoid() || rightType->isVoid()) {
    auto message = this->operandTypesText("??", leftType, rightType);

    return err(CodeLogicRegistryCheckNullishError{
        .code = CodeLogicRegistryCheckNullishErrorCode::VOID_OPERAND,
        .message = message,
    });
  }
  if (leftType->isUndefined()) {
    return ok(rightType);
  }
  const Type *base = rightType->isUndefined()
                         ? leftType->nonOptional()
                         : this->state.types.literals.unionUnify(leftType->nonOptional(),
                                                                 rightType->nonOptional());
  if (base == nullptr || base->isError()) {
    auto message = this->operandTypesText("??", leftType, rightType);

    return err(CodeLogicRegistryCheckNullishError{
        .code = CodeLogicRegistryCheckNullishErrorCode::NO_COMMON_TYPE,
        .message = message,
    });
  }
  const Type *result =
      rightType->isOptional() || rightType->isUndefined() ? Type::optional(base) : base;

  return ok(result);
}

std::string NullishValues::operandTypesText(llvm::StringRef op, const Type *lhs,
                                            const Type *rhs) const {
  bool isUndefined =
      lhs->isOptional() || rhs->isOptional() || lhs->isUndefined() || rhs->isUndefined();

  return ErrorTextOperands::types(op, lhs->name(), rhs->name(), isUndefined);
}

const Type *NullishValues::failedNullishType(const Type *leftType, const Type *rightType) const {
  bool isOneSideKnown =
      this->state.walk.functions.isInsideTrial() && leftType->isError() != rightType->isError();
  if (!isOneSideKnown) {
    return Type::error();
  }

  return leftType->isError() ? rightType : leftType->nonOptional();
}

}
