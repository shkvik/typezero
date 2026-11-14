#include "compiler/semantic/code/arithmetic/arithmetic_operators/arithmetic_operators.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/CheckedArithmetic.h"
#include <cstdio>
#include <limits>

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

ArithmeticOperators::ArithmeticOperators(SemanticState &owner,
                                         ArithmeticRules &sharedArithmeticRules)
    : state(owner),
      arithmeticRules(sharedArithmeticRules) {}

void ArithmeticOperators::toInt32() {
  Expr *checked = this->state.walk.tasks.takeChecked();
  this->state.walk.tasks.pushChecked(checked);
  bool isKept = checked->type->isInt() || checked->type->isError();
  if (!isKept) {
    this->state.walk.tasks.pushTask(Convert{.to = Type::intTy()});
  }
}

void ArithmeticOperators::noteUnsignedVar(const NoteUnsignedVar &task) {
  if (!this->arithmeticRules.isUnsigned64(task.variable->type)) {
    return;
  }
  auto value = this->arithmeticRules.unsignedConstant(task.init);
  if (value.has_value() && *value > uint64_t(std::numeric_limits<int64_t>::max())) {
    this->state.code.constants.rememberBigUnsignedVar(task.variable, *value);
  }
}

void ArithmeticOperators::checkShift(const CheckShift &task) {
  BinaryExpr *binary = task.binary;
  bool isIntegerExpected = task.expected != nullptr && task.expected->isInteger();
  const Type *hint = isIntegerExpected ? task.expected : nullptr;
  const Type *lhsHint = this->hasOnlyIntLiterals(binary->lhs) ? hint : nullptr;
  this->state.walk.tasks.pushTask(StartBits{.binary = binary});
  this->state.walk.tasks.pushTask(ApparentSlot{.slot = &binary->rhs});
  this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &binary->rhs, .expected = nullptr});
  this->state.walk.tasks.pushTask(ApparentSlot{.slot = &binary->lhs});

  return this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &binary->lhs, .expected = lhsHint});
}

void ArithmeticOperators::checkArithmetic(const CheckArithmetic &task) {
  BinaryExpr *binary = task.binary;
  auto &concatenation = this->state.code.concatenation;
  auto *chain = concatenation.takeConcatChain();
  bool isChainEnd = binary->op == BinaryOp::Add && chain == nullptr;
  const BinaryExpr *outerMixed = isChainEnd ? concatenation.takeMixedConcat() : nullptr;
  auto *left = dyn_cast<BinaryExpr>(binary->lhs);
  bool isChainGoesLeft =
      binary->op == BinaryOp::Add && left != nullptr && left->op == BinaryOp::Add;
  if (isChainGoesLeft) {
    BinaryExpr *chainStart = chain != nullptr ? chain : binary;
    concatenation.rememberConcatChain(chainStart);
  }
  auto hints = this->operandHints(binary, task.expected);
  const auto &conversions = this->state.code.conversions;
  bool isLiteralLeft = conversions.isNumberLiteralTree(binary->lhs) &&
                       !conversions.isNumberLiteralTree(binary->rhs) &&
                       (hints.canHintLiterals || this->hasOnlyIntLiterals(binary->lhs));
  auto draft = ArithmeticDraft{
      .binary = binary,
      .hint = hints.hint,
      .rhsHint = hints.rhsHint,
      .isBitwise = hints.isBitwise,
      .canHintLiterals = hints.canHintLiterals,
      .pending = nullptr,
      .isChainEnd = isChainEnd,
      .outerMixed = outerMixed,
      .isForgettingChain = false,
  };
  if (isLiteralLeft) {
    draft.pending = concatenation.takeConcatChain();
    draft.isForgettingChain = true;
    this->state.walk.tasks.pushTask(AfterArithmeticRight{.draft = draft});
    this->state.walk.tasks.pushTask(ApparentSlot{.slot = &binary->rhs});
    return this->state.walk.tasks.pushTask(
        CheckExprSlot{.slot = &binary->rhs, .expected = hints.rhsHint});
  }
  this->state.walk.tasks.pushTask(AfterArithmeticLeft{.draft = draft});
  this->state.walk.tasks.pushTask(ApparentSlot{.slot = &binary->lhs});

  return this->state.walk.tasks.pushTask(
      CheckExprSlot{.slot = &binary->lhs, .expected = hints.hint});
}

void ArithmeticOperators::afterArithmeticRight(const AfterArithmeticRight &task) {
  const ArithmeticDraft &draft = task.draft;
  BinaryExpr *binary = draft.binary;
  this->state.code.concatenation.rememberConcatChain(draft.pending);
  const Type *literalHint = draft.canHintLiterals
                                ? this->state.code.conversions.literalHint(binary->rhs, draft.hint)
                                : draft.hint;
  const auto *lhsHint = this->integerHint(binary->lhs, binary->rhs, literalHint, draft.isBitwise);
  this->state.walk.tasks.pushTask(AfterArithmeticOperands{.draft = draft});
  this->state.walk.tasks.pushTask(ApparentSlot{.slot = &binary->lhs});

  return this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &binary->lhs, .expected = lhsHint});
}

void ArithmeticOperators::afterArithmeticLeft(const AfterArithmeticLeft &task) {
  const ArithmeticDraft &draft = task.draft;
  BinaryExpr *binary = draft.binary;
  this->state.code.concatenation.forgetConcatChain();
  bool isLiteralRight =
      draft.canHintLiterals && this->state.code.conversions.isNumberLiteralTree(binary->rhs);
  const Type *literalHint =
      isLiteralRight ? this->state.code.conversions.literalHint(binary->lhs, draft.rhsHint)
                     : draft.rhsHint;
  const auto *rhsHint = this->integerHint(binary->rhs, binary->lhs, literalHint, draft.isBitwise);
  this->state.walk.tasks.pushTask(AfterArithmeticOperands{.draft = draft});
  this->state.walk.tasks.pushTask(ApparentSlot{.slot = &binary->rhs});

  return this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &binary->rhs, .expected = rhsHint});
}

void ArithmeticOperators::afterArithmeticOperands(const AfterArithmeticOperands &task) {
  if (task.draft.isForgettingChain) {
    this->state.code.concatenation.forgetConcatChain();
  }
  this->state.walk.tasks.pushTask(FinishArithmetic{.draft = task.draft});

  return this->state.walk.tasks.pushTask(NeverOperands{.binary = task.draft.binary});
}

void ArithmeticOperators::finishArithmetic(const FinishArithmetic &task) {
  this->state.walk.tasks.pushTask(ResolveArithmetic{.draft = task.draft});

  return this->pushLiteralAdapting(task.draft.binary);
}

void ArithmeticOperators::resolveArithmetic(const ResolveArithmetic &task) {
  BinaryExpr *binary = task.draft.binary;
  this->state.walk.tasks.pushTask(ArithmeticResult{.binary = binary});
  if (binary->op == BinaryOp::Add) {
    this->state.walk.tasks.pushTask(TrackMixedConcat{
        .binary = binary,
        .isChainEnd = task.draft.isChainEnd,
        .outerMixed = task.draft.outerMixed,
    });
  }
}

void ArithmeticOperators::arithmeticResult(const ArithmeticResult &task) {
  BinaryExpr *binary = task.binary;
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  binary->type = Type::error();
  if (leftType->isError() || rightType->isError()) {
    return this->state.walk.tasks.pushChecked(binary);
  }
  bool isConcatenating =
      binary->op == BinaryOp::Add && (leftType->isString() || rightType->isString());
  if (isConcatenating) {
    return this->state.walk.tasks.pushTask(CheckConcatenation{.binary = binary});
  }
  if (leftType->isDate() || rightType->isDate()) {
    return this->state.walk.tasks.pushTask(CheckDateOperands{.binary = binary});
  }
  if (this->isBitwiseOperator(binary->op)) {
    auto bits = StartBits{.binary = binary};
    return this->startBits(bits);
  }
  if (leftType->isDecimal() || rightType->isDecimal()) {
    return this->state.walk.tasks.pushTask(CheckDecimalArithmetic{.binary = binary});
  }
  bool isWrapping = (leftType->isWrapping() || rightType->isWrapping()) && leftType->isInteger() &&
                    rightType->isInteger();
  if (isWrapping) {
    return this->arithmeticRules.startWrapping(binary);
  }

  return this->startNumberArithmetic(binary);
}

void ArithmeticOperators::startNumberArithmetic(BinaryExpr *binary) {
  const Type *leftType = binary->lhs->type;
  const Type *writtenRightType = binary->rhs->type;
  bool isFloatPower =
      binary->op == BinaryOp::Pow && leftType->isFloat() && writtenRightType->isInteger();
  const Type *rightType = isFloatPower ? leftType : writtenRightType;
  bool hasNumbers = leftType->isNumeric() && rightType->isNumeric();
  const Type *common =
      hasNumbers ? this->state.types.relations.commonNumericType(leftType, rightType) : nullptr;
  if (hasNumbers && common == nullptr) {
    auto message = this->arithmeticRules.mixedNumbersText(binary);
    this->state.diagnostics.error(binary->loc(), message);
    return this->state.walk.tasks.pushChecked(binary);
  }
  if (common == nullptr) {
    const char *op = Operators::opSpelling(binary->op);
    auto message = this->arithmeticRules.operandTypesText(op, leftType, rightType);
    this->state.diagnostics.error(binary->loc(), message);
    return this->state.walk.tasks.pushChecked(binary);
  }
  binary->type = common;
  this->state.walk.tasks.pushChecked(binary);
  if (common->isInteger()) {
    this->state.walk.tasks.pushTask(FinishIntegerArithmetic{.binary = binary, .common = common});
  }
  this->state.walk.tasks.pushTask(ConvertSlot{.slot = &binary->rhs, .to = common});
  this->state.walk.tasks.pushTask(ConvertSlot{.slot = &binary->lhs, .to = common});
  if (isFloatPower) {
    this->state.walk.tasks.pushTask(ConvertSlot{.slot = &binary->rhs, .to = leftType});
  }
}

void ArithmeticOperators::finishIntegerArithmetic(const FinishIntegerArithmetic &task) {
  BinaryExpr *binary = task.binary;
  if (binary->op == BinaryOp::Pow) {
    auto power = this->checkPowerExponent(binary->rhs, task.common);
    if (power.isErr()) {
      this->state.diagnostics.error(binary->rhs->loc(), power.error().message);
      return;
    }
  }
  auto divisor = this->checkDivisor(binary->op, binary->rhs);
  if (divisor.isErr()) {
    this->state.diagnostics.error(binary->rhs->loc(), divisor.error().message);
  }

  return this->arithmeticRules.foldBinary(binary);
}

void ArithmeticOperators::checkWrappingDivisor(const CheckWrappingDivisor &task) {
  auto divisor = this->checkDivisor(task.binary->op, task.binary->rhs);
  if (divisor.isErr()) {
    this->state.diagnostics.error(task.binary->rhs->loc(), divisor.error().message);
  }
}

void ArithmeticOperators::startBits(const StartBits &task) {
  this->state.walk.tasks.pushTask(FinishBits{.binary = task.binary});

  return this->pushLiteralAdapting(task.binary);
}

void ArithmeticOperators::foldBinaryNode(const FoldBinaryNode &task) {
  return this->arithmeticRules.foldBinary(task.binary);
}

void ArithmeticOperators::pushLiteralAdapting(BinaryExpr *binary) {
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  if (leftType == nullptr || rightType == nullptr || leftType == rightType || leftType->isError() ||
      rightType->isError()) {
    return;
  }
  bool isBitwise =
      this->isBitwiseOperator(binary->op) || this->arithmeticRules.isShiftOperator(binary->op);
  bool hasNumbers = this->isNumberLike(leftType) && this->isNumberLike(rightType);
  bool hasIntegers = leftType->isInteger() && rightType->isInteger();
  if (!hasNumbers || (isBitwise && !hasIntegers)) {
    return;
  }
  const auto &conversions = this->state.code.conversions;
  bool isPower = binary->op == BinaryOp::Pow;
  bool isLeftAdapting =
      conversions.isLiteralAdapting(binary->lhs, rightType) && !(isPower && rightType->isDecimal());
  if (isLeftAdapting) {
    return this->state.walk.tasks.pushTask(AdaptLiteralSlot{.slot = &binary->lhs, .to = rightType});
  }
  bool isRightAdapting = conversions.isLiteralAdapting(binary->rhs, leftType) &&
                         !(isPower && (leftType->isDecimal() || leftType->isFloat()));
  if (isRightAdapting) {
    this->state.walk.tasks.pushTask(AdaptLiteralSlot{.slot = &binary->rhs, .to = leftType});
  }
}

bool ArithmeticOperators::hasOnlyIntLiterals(const Expr *expr) const {
  for (const Expr *node : ExprParts::operatorTree(expr)) {
    if (!this->isIntLiteralNode(node)) {
      return false;
    }
  }

  return true;
}

bool ArithmeticOperators::isIntLiteralNode(const Expr *expr) const {
  if (isa<IntLitExpr>(expr)) {
    return true;
  }
  auto *unary = dyn_cast<UnaryExpr>(expr);
  if (unary != nullptr) {
    return unary->op == UnaryOp::Neg || unary->op == UnaryOp::Plus;
  }
  auto *binary = dyn_cast<BinaryExpr>(expr);
  if (binary != nullptr) {
    return binary->op == BinaryOp::Add || binary->op == BinaryOp::Sub ||
           binary->op == BinaryOp::Mul || binary->op == BinaryOp::Rem ||
           binary->op == BinaryOp::Pow;
  }

  return false;
}

const Type *ArithmeticOperators::integerHint(const Expr *literal, const Expr *other,
                                             const Type *hint, bool isBitwise) const {
  const Type *type = other->type != nullptr ? other->type->nonOptional() : nullptr;
  bool isOtherNumber = type != nullptr && (type->isInteger() || (type->isFloat() && !isBitwise));
  bool isTakingOther = isOtherNumber && this->hasOnlyIntLiterals(literal) &&
                       !this->state.code.conversions.isIntLiteralTree(other);
  if (isTakingOther) {
    return type;
  }

  return hint;
}

ArithmeticOperators::ResultExponentIsValid
ArithmeticOperators::checkPowerExponent(const Expr *rhs, const Type *common) const {
  auto exponent = this->state.code.constants.intConstant(rhs);
  if (exponent && *exponent < 0) {
    auto message = ErrorTextPower::negative(common->name());

    return err(CodeArithmeticRegistryPowerExponentError{
        .code = CodeArithmeticRegistryPowerExponentErrorCode::NEGATIVE_POWER,
        .message = message,
    });
  }

  return ok(rhs);
}

bool ArithmeticOperators::isBitwiseOperator(BinaryOp op) const {
  return op == BinaryOp::BitAnd || op == BinaryOp::BitOr || op == BinaryOp::BitXor;
}

ArithmeticOperators::OperandHints ArithmeticOperators::operandHints(const BinaryExpr *binary,
                                                                    const Type *expected) const {
  bool isBitwise = this->isBitwiseOperator(binary->op);
  bool isPower = binary->op == BinaryOp::Pow;
  bool isNumberExpected =
      expected != nullptr && !isBitwise && (expected->isNumeric() || expected->isDecimal());
  const Type *hint = isNumberExpected ? expected : nullptr;
  bool isDecimalPower = isPower && hint != nullptr && hint->isDecimal();

  return OperandHints{
      .hint = hint,
      .rhsHint = isDecimalPower ? nullptr : hint,
      .isBitwise = isBitwise,
      .canHintLiterals = !isBitwise && !isPower,
  };
}

bool ArithmeticOperators::isNumberLike(const Type *type) const {
  return type->isNumeric() || type->isDecimal();
}

ArithmeticOperators::ResultDivisorNotZero
ArithmeticOperators::checkDivisor(BinaryOp op, const Expr *divisor) const {
  bool isDividing = op == BinaryOp::Div || op == BinaryOp::Rem;
  if (isDividing && this->state.code.constants.intConstant(divisor) == 0) {
    auto message = ErrorTextDivisor::zero();

    return err(CodeArithmeticRegistryCheckDivisorError{
        .code = CodeArithmeticRegistryCheckDivisorErrorCode::DIVISION_BY_ZERO,
        .message = message,
    });
  }

  return ok(divisor);
}

}
