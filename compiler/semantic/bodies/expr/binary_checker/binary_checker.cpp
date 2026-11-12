#include "compiler/semantic/bodies/expr/binary_checker/binary_checker.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/program/types/type_spelling/type_spelling.h"
#include "compiler/semantic/bodies/classes/instance_of_checker/instance_of_checker.h"
#include "compiler/semantic/bodies/expr/concat_checker/concat_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/expr/truth_checker/truth_checker.h"
#include "compiler/semantic/bodies/expr/unary_checker/unary_checker.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/library/date/date_checker/date_checker.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker_errors.h"
#include "compiler/semantic/library/json/json_checker/json_checker.h"
#include "compiler/semantic/library/regex/match_checker/match_checker.h"
#include "compiler/semantic/library/result/result_types/result_types.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr const char *kValuePlaceholder = "x";
constexpr const char *kCountPlaceholder = "n";
constexpr const char *kBigIntTypeOf = "bigint";
constexpr const char *kNumberTypeOf = "number";
constexpr const char *kSymbolTypeOf = "symbol";
constexpr size_t kMostShownNumberNames = 3;

}

BinaryChecker::BinaryChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->dateChecker = &all.dateChecker;
  this->exprChecker = &all.exprChecker;
  this->jsonChecker = &all.jsonChecker;
  this->resultTypes = &all.resultTypes;
  this->flowChecker = &all.flowChecker;
  this->matchChecker = &all.matchChecker;
  this->truthChecker = &all.truthChecker;
  this->unaryChecker = &all.unaryChecker;
  this->concatChecker = &all.concatChecker;
  this->typeRelations = &all.typeRelations;
  this->constantFolder = &all.constantFolder;
  this->decimalChecker = &all.decimalChecker;
  this->genericChecker = &all.genericChecker;
  this->instanceOfChecker = &all.instanceOfChecker;
}

bool BinaryChecker::hasOnlyIntLiterals(const Expr *expr) const {
  for (const Expr *node : ExprParts::operatorTree(expr)) {
    if (!this->isIntLiteralNode(node)) {
      return false;
    }
  }

  return true;
}

bool BinaryChecker::isIntLiteralNode(const Expr *expr) const {
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

const Type *BinaryChecker::integerHint(const Expr *literal, const Expr *other, const Type *hint,
                                       bool isBitwise) const {
  const Type *type = other->type != nullptr ? other->type->nonOptional() : nullptr;
  bool isOtherNumber = type != nullptr && (type->isInteger() || (type->isFloat() && !isBitwise));
  bool isTakingOther = isOtherNumber && this->hasOnlyIntLiterals(literal) &&
                       !this->typeRelations->isIntLiteralTree(other);
  if (isTakingOther) {
    return type;
  }

  return hint;
}

std::string BinaryChecker::operandTypesText(llvm::StringRef op, const Type *lhs,
                                            const Type *rhs) const {
  bool isUndefined =
      lhs->isOptional() || rhs->isOptional() || lhs->isUndefined() || rhs->isUndefined();

  return ErrorTextOperands::types(op, lhs->name(), rhs->name(), isUndefined);
}

BinaryChecker::ResultExponentIsValid BinaryChecker::checkPowerExponent(const Expr *rhs,
                                                                       const Type *common) const {
  auto exponent = this->constantFolder->intConstant(rhs);
  if (exponent && *exponent < 0) {
    auto message = ErrorTextPower::negative(common->name());

    return err(BinaryCheckerPowerExponentError{
        .code = BinaryCheckerPowerExponentErrorCode::NEGATIVE_POWER,
        .message = message,
    });
  }

  return ok(rhs);
}

Expr *BinaryChecker::checkBinary(BinaryExpr *binary, const Type *expected) {
  const Type *expectedValue = expected != nullptr ? expected->nonOptional() : nullptr;
  switch (binary->op) {
    case BinaryOp::In:
      return this->checkInOperator(binary);
    case BinaryOp::InstanceOf:
      return this->checkInstanceOfOperator(binary);
    case BinaryOp::LogAnd:
    case BinaryOp::LogOr:
      return this->checkLogicalOperator(binary, expectedValue);
    case BinaryOp::Nullish:
      return this->checkNullishOperator(binary, expectedValue);
    case BinaryOp::Eq:
    case BinaryOp::Ne:
      return this->checkEquality(binary);
    case BinaryOp::Lt:
    case BinaryOp::Le:
    case BinaryOp::Gt:
    case BinaryOp::Ge:
      return this->checkComparison(binary);
    case BinaryOp::Shl:
    case BinaryOp::Shr:
    case BinaryOp::UShr:
      return this->checkShift(binary, expectedValue);
    case BinaryOp::Add:
    case BinaryOp::Sub:
    case BinaryOp::Mul:
    case BinaryOp::Div:
    case BinaryOp::Rem:
    case BinaryOp::Pow:
    case BinaryOp::BitAnd:
    case BinaryOp::BitOr:
    case BinaryOp::BitXor:
      return this->checkArithmetic(binary, expectedValue);
  }

  return binary;
}

Expr *BinaryChecker::checkInOperator(BinaryExpr *binary) {
  auto in = this->unaryChecker->checkIn(binary);
  if (in.isErr()) {
    auto at = this->inErrorPlace(binary, in.error().code);
    this->state.getDiagnostics().error(at, in.error().message);
    return binary;
  }

  return in.value();
}

llvm::SMLoc BinaryChecker::inErrorPlace(const BinaryExpr *binary,
                                        UnaryCheckerCheckInErrorCode code) const {
  switch (code) {
    case UnaryCheckerCheckInErrorCode::DICT_UNDEFINED:
    case UnaryCheckerCheckInErrorCode::UNION_UNDEFINED:
      return binary->rhs->loc();
    case UnaryCheckerCheckInErrorCode::DICT_NUMBER_KEY:
      return binary->lhs->loc();
    case UnaryCheckerCheckInErrorCode::DICT_NOT_RECORD:
    case UnaryCheckerCheckInErrorCode::UNION_NO_KEY:
    case UnaryCheckerCheckInErrorCode::OPTIONAL_FIELD:
    case UnaryCheckerCheckInErrorCode::ALWAYS_SAME:
    case UnaryCheckerCheckInErrorCode::TUPLE_IN:
    case UnaryCheckerCheckInErrorCode::OBJECT_OPTIONAL:
    case UnaryCheckerCheckInErrorCode::OBJECT_ALWAYS_TRUE:
    case UnaryCheckerCheckInErrorCode::OBJECT_ALWAYS_FALSE:
    case UnaryCheckerCheckInErrorCode::OBJECT_IN:
    case UnaryCheckerCheckInErrorCode::OPERAND_TYPES:
    case UnaryCheckerCheckInErrorCode::ARRAY_IN:
      return binary->loc();
  }

  return binary->loc();
}

Expr *BinaryChecker::checkInstanceOfOperator(BinaryExpr *binary) {
  auto tested = this->instanceOfChecker->checkInstanceOf(binary);
  if (tested.isErr()) {
    auto at = this->instanceOfErrorPlace(binary, tested.error().code);
    this->state.getDiagnostics().error(at, tested.error().message);
    return binary;
  }

  return tested.value();
}

llvm::SMLoc
BinaryChecker::instanceOfErrorPlace(const BinaryExpr *binary,
                                    InstanceOfCheckerCheckInstanceOfErrorCode code) const {
  switch (code) {
    case InstanceOfCheckerCheckInstanceOfErrorCode::NOT_AN_OBJECT:
      return binary->lhs->loc();
    case InstanceOfCheckerCheckInstanceOfErrorCode::NOT_A_CLASS:
      return binary->rhs->loc();
    case InstanceOfCheckerCheckInstanceOfErrorCode::UNTYPED_COLLECTION:
    case InstanceOfCheckerCheckInstanceOfErrorCode::NONE_MATCHES:
    case InstanceOfCheckerCheckInstanceOfErrorCode::ALL_MATCH:
    case InstanceOfCheckerCheckInstanceOfErrorCode::ALWAYS_TRUE:
    case InstanceOfCheckerCheckInstanceOfErrorCode::TEST_UNDEFINED:
    case InstanceOfCheckerCheckInstanceOfErrorCode::ALWAYS_FALSE:
    case InstanceOfCheckerCheckInstanceOfErrorCode::BYTES_INSTANCE:
    case InstanceOfCheckerCheckInstanceOfErrorCode::GENERIC_UNKNOWN:
    case InstanceOfCheckerCheckInstanceOfErrorCode::NEVER_GENERIC:
    case InstanceOfCheckerCheckInstanceOfErrorCode::NEVER_IN_UNION:
    case InstanceOfCheckerCheckInstanceOfErrorCode::NEVER_CLASS:
    case InstanceOfCheckerCheckInstanceOfErrorCode::NOT_IMPLEMENTED:
    case InstanceOfCheckerCheckInstanceOfErrorCode::NEVER_CREATED:
      return binary->loc();
  }

  return binary->loc();
}

Expr *BinaryChecker::checkLogicalOperator(BinaryExpr *binary, const Type *expected) {
  auto whenTrue = Flow();
  auto whenFalse = Flow();
  TruthUse use = binary->op == BinaryOp::LogAnd ? TruthUse::And : TruthUse::Or;
  this->truthChecker->checkLogical(binary, expected, whenTrue, whenFalse, RightSide::VALUE, use);
  auto joined = this->flowChecker->join(whenTrue, whenFalse);
  this->state.getFlowState().replaceFlow(std::move(joined));

  return binary;
}

Expr *BinaryChecker::checkNullishOperator(BinaryExpr *binary, const Type *expected) {
  auto nullish = this->checkNullish(binary, expected);
  if (nullish.isErr()) {
    this->state.getDiagnostics().error(binary->loc(), nullish.error().message);
    return binary;
  }

  return nullish.value();
}

Expr *BinaryChecker::checkComparison(BinaryExpr *binary) {
  this->decimalChecker->walkComparisonOperands(binary);
  this->resultTypes->neverOperands(binary);
  binary->type = Type::boolTy();
  bool isSameParam = binary->lhs->type->isParam() && binary->lhs->type == binary->rhs->type;
  if (isSameParam) {
    return binary;
  }

  binary->lhs = this->genericChecker->apparent(binary->lhs);
  binary->rhs = this->genericChecker->apparent(binary->rhs);
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  if (leftType->isError() || rightType->isError()) {
    return binary;
  }
  if (leftType->isString() && rightType->isString()) {
    return binary;
  }
  auto *dates = this->dateChecker->checkDateOperands(binary);
  if (dates != nullptr) {
    return dates;
  }
  if (leftType->isDecimal() || rightType->isDecimal()) {
    return this->decimalChecker->checkDecimalComparison(binary);
  }
  if (leftType->isNumeric() && rightType->isNumeric()) {
    return this->checkNumberComparison(binary);
  }
  const char *op = Operators::opSpelling(binary->op);
  auto message = this->operandTypesText(op, leftType, rightType);
  this->state.getDiagnostics().error(binary->loc(), message);

  return binary;
}

Expr *BinaryChecker::checkNumberComparison(BinaryExpr *binary) {
  this->adaptLiterals(binary);
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  bool isComparable = leftType == rightType || (leftType->isInteger() && rightType->isInteger());
  if (!isComparable) {
    auto message = this->mixedNumbersText(binary);
    this->state.getDiagnostics().error(binary->loc(), message);
  }

  return binary;
}

Expr *BinaryChecker::checkShift(BinaryExpr *binary, const Type *expected) {
  bool isIntegerExpected = expected != nullptr && expected->isInteger();
  const Type *hint = isIntegerExpected ? expected : nullptr;
  const Type *lhsHint = this->hasOnlyIntLiterals(binary->lhs) ? hint : nullptr;
  auto *lhs = this->exprChecker->checkExpr(binary->lhs, lhsHint);
  binary->lhs = this->genericChecker->apparent(lhs);
  auto *rhs = this->exprChecker->checkExpr(binary->rhs);
  binary->rhs = this->genericChecker->apparent(rhs);

  return this->checkBitwiseOperator(binary);
}

Expr *BinaryChecker::checkBitwiseOperator(BinaryExpr *binary) {
  auto bits = this->checkBits(binary);
  if (bits.isErr()) {
    bool isDivisor = bits.error().code == BinaryCheckerCheckBitsErrorCode::DIVISOR_ZERO;
    llvm::SMLoc at = isDivisor ? binary->rhs->loc() : binary->loc();
    this->state.getDiagnostics().error(at, bits.error().message);
    return binary;
  }

  return bits.value();
}

bool BinaryChecker::isBitwiseOperator(BinaryOp op) const {
  return op == BinaryOp::BitAnd || op == BinaryOp::BitOr || op == BinaryOp::BitXor;
}

bool BinaryChecker::isShiftOperator(BinaryOp op) const {
  return op == BinaryOp::Shl || op == BinaryOp::Shr || op == BinaryOp::UShr;
}

Expr *BinaryChecker::checkArithmetic(BinaryExpr *binary, const Type *expected) {
  auto &marks = this->state.getExpressionMarks();
  auto *chain = marks.takeConcatChain();
  bool isChainEnd = binary->op == BinaryOp::Add && chain == nullptr;
  const BinaryExpr *outerMixed = isChainEnd ? marks.takeMixedConcat() : nullptr;
  auto *left = dyn_cast<BinaryExpr>(binary->lhs);
  bool isChainGoesLeft =
      binary->op == BinaryOp::Add && left != nullptr && left->op == BinaryOp::Add;
  if (isChainGoesLeft) {
    BinaryExpr *chainStart = chain != nullptr ? chain : binary;
    marks.rememberConcatChain(chainStart);
  }

  this->walkArithmeticOperands(binary, expected);
  this->resultTypes->neverOperands(binary);
  this->adaptLiterals(binary);
  if (binary->op == BinaryOp::Add) {
    this->trackMixedConcat(binary, isChainEnd, outerMixed);
  }

  return this->checkArithmeticResult(binary);
}

BinaryChecker::OperandHints BinaryChecker::operandHints(const BinaryExpr *binary,
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

void BinaryChecker::walkArithmeticOperands(BinaryExpr *binary, const Type *expected) {
  auto hints = this->operandHints(binary, expected);
  bool isLiteralLeft = this->decimalChecker->isLiteralTree(binary->lhs) &&
                       !this->decimalChecker->isLiteralTree(binary->rhs) &&
                       (hints.canHintLiterals || this->hasOnlyIntLiterals(binary->lhs));
  if (isLiteralLeft) {
    return this->walkRightOperandFirst(binary, hints);
  }

  return this->walkLeftOperandFirst(binary, hints);
}

void BinaryChecker::walkRightOperandFirst(BinaryExpr *binary, const OperandHints &hints) {
  auto &marks = this->state.getExpressionMarks();
  auto *pending = marks.takeConcatChain();
  auto *rhs = this->exprChecker->checkExpr(binary->rhs, hints.rhsHint);
  binary->rhs = this->genericChecker->apparent(rhs);
  marks.rememberConcatChain(pending);
  const Type *literalHint = hints.canHintLiterals
                                ? this->decimalChecker->literalHint(binary->rhs, hints.hint)
                                : hints.hint;
  const auto *lhsHint = this->integerHint(binary->lhs, binary->rhs, literalHint, hints.isBitwise);
  auto *lhs = this->exprChecker->checkExpr(binary->lhs, lhsHint);
  binary->lhs = this->genericChecker->apparent(lhs);
  marks.forgetConcatChain();
}

void BinaryChecker::walkLeftOperandFirst(BinaryExpr *binary, const OperandHints &hints) {
  auto &marks = this->state.getExpressionMarks();
  auto *lhs = this->exprChecker->checkExpr(binary->lhs, hints.hint);
  binary->lhs = this->genericChecker->apparent(lhs);
  marks.forgetConcatChain();
  bool isLiteralRight = hints.canHintLiterals && this->decimalChecker->isLiteralTree(binary->rhs);
  const Type *literalHint = isLiteralRight
                                ? this->decimalChecker->literalHint(binary->lhs, hints.rhsHint)
                                : hints.rhsHint;
  const auto *rhsHint = this->integerHint(binary->rhs, binary->lhs, literalHint, hints.isBitwise);
  auto *rhs = this->exprChecker->checkExpr(binary->rhs, rhsHint);
  binary->rhs = this->genericChecker->apparent(rhs);
}

void BinaryChecker::trackMixedConcat(BinaryExpr *binary, bool isChainEnd,
                                     const BinaryExpr *outerMixed) {
  auto &marks = this->state.getExpressionMarks();
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  bool isMixing = (leftType->isString() && this->isJoiningAsText(rightType)) ||
                  (rightType->isString() && this->isJoiningAsText(leftType));
  if (marks.getMixedConcat() == nullptr && isMixing) {
    marks.rememberMixedConcat(binary);
  }
  if (!isChainEnd) {
    return;
  }
  const auto *first = marks.getMixedConcat();
  if (first != nullptr) {
    auto mixed = this->concatChecker->mixedConcatText(binary, first);
    this->state.getDiagnostics().error(first->loc(), mixed);
  }
  marks.rememberMixedConcat(outerMixed);
}

bool BinaryChecker::isJoiningAsText(const Type *type) const {
  const Type *base = type->nonOptional();

  return base->isNumeric() || base->isDecimal() || base->isBool();
}

Expr *BinaryChecker::checkArithmeticResult(BinaryExpr *binary) {
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  binary->type = Type::error();
  if (leftType->isError() || rightType->isError()) {
    return binary;
  }
  bool isConcatenating =
      binary->op == BinaryOp::Add && (leftType->isString() || rightType->isString());
  if (isConcatenating) {
    return this->checkConcatenation(binary);
  }
  auto *dates = this->dateChecker->checkDateOperands(binary);
  if (dates != nullptr) {
    return dates;
  }
  if (this->isBitwiseOperator(binary->op)) {
    return this->checkBitwiseOperator(binary);
  }
  if (leftType->isDecimal() || rightType->isDecimal()) {
    return this->decimalChecker->checkDecimalArithmetic(binary);
  }
  bool isWrapping = (leftType->isWrapping() || rightType->isWrapping()) && leftType->isInteger() &&
                    rightType->isInteger();
  if (isWrapping) {
    return this->checkWrappingArithmetic(binary);
  }

  return this->checkNumberArithmetic(binary);
}

Expr *BinaryChecker::checkConcatenation(BinaryExpr *binary) {
  binary->lhs = this->concatChecker->toStringOperand(binary->lhs);
  binary->rhs = this->concatChecker->toStringOperand(binary->rhs);
  binary->type = Type::string();

  return binary;
}

Expr *BinaryChecker::checkWrappingArithmetic(BinaryExpr *binary) {
  auto wrapped = this->checkWrapping(binary);
  if (wrapped.isErr()) {
    bool isDivisor = wrapped.error().code == BinaryCheckerCheckWrappingErrorCode::DIVISOR_ZERO;
    llvm::SMLoc at = isDivisor ? binary->rhs->loc() : binary->loc();
    this->state.getDiagnostics().error(at, wrapped.error().message);
    return binary;
  }

  return wrapped.value();
}

Expr *BinaryChecker::checkNumberArithmetic(BinaryExpr *binary) {
  const Type *leftType = binary->lhs->type;
  const Type *writtenRightType = binary->rhs->type;
  bool isFloatPower =
      binary->op == BinaryOp::Pow && leftType->isFloat() && writtenRightType->isInteger();
  if (isFloatPower) {
    binary->rhs = this->typeRelations->convert(binary->rhs, leftType);
  }
  const Type *rightType = isFloatPower ? leftType : writtenRightType;
  bool hasNumbers = leftType->isNumeric() && rightType->isNumeric();
  const Type *common = hasNumbers ? TypeRelations::commonNumericType(leftType, rightType) : nullptr;
  if (hasNumbers && common == nullptr) {
    auto message = this->mixedNumbersText(binary);
    this->state.getDiagnostics().error(binary->loc(), message);
    return binary;
  }
  if (common == nullptr) {
    const char *op = Operators::opSpelling(binary->op);
    auto message = this->operandTypesText(op, leftType, rightType);
    this->state.getDiagnostics().error(binary->loc(), message);
    return binary;
  }

  binary->lhs = this->typeRelations->convert(binary->lhs, common);
  binary->rhs = this->typeRelations->convert(binary->rhs, common);
  binary->type = common;
  if (!common->isInteger()) {
    return binary;
  }

  return this->checkIntegerArithmetic(binary, common);
}

Expr *BinaryChecker::checkIntegerArithmetic(BinaryExpr *binary, const Type *common) {
  if (binary->op == BinaryOp::Pow) {
    auto power = this->checkPowerExponent(binary->rhs, common);
    if (power.isErr()) {
      this->state.getDiagnostics().error(binary->rhs->loc(), power.error().message);
      return binary;
    }
  }
  auto divisor = this->constantFolder->checkDivisor(binary->op, binary->rhs);
  if (divisor.isErr()) {
    this->state.getDiagnostics().error(binary->rhs->loc(), divisor.error().message);
  }
  this->constantFolder->foldBinary(binary);

  return binary;
}

BinaryChecker::ResultWrappingChecked BinaryChecker::checkWrapping(BinaryExpr *binary) {
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  if (leftType->isWrapping() && rightType->isWrapping() && leftType != rightType) {
    auto message = this->mixedNumbersText(binary);

    return err(BinaryCheckerCheckWrappingError{
        .code = BinaryCheckerCheckWrappingErrorCode::MIXED_NUMBERS,
        .message = message,
    });
  }
  const Type *wrappingType = leftType->isWrapping() ? leftType : rightType;
  binary->lhs = this->typeRelations->convert(binary->lhs, wrappingType);
  binary->rhs = this->typeRelations->convert(binary->rhs, wrappingType);
  binary->type = wrappingType;
  auto divisor = this->constantFolder->checkDivisor(binary->op, binary->rhs);
  if (divisor.isErr()) {
    return err(BinaryCheckerCheckWrappingError{
        .code = BinaryCheckerCheckWrappingErrorCode::DIVISOR_ZERO,
        .message = divisor.error().message,
    });
  }

  return ok(binary);
}

BinaryChecker::ResultBitwiseResolved BinaryChecker::checkBits(BinaryExpr *binary) {
  bool isShift = this->isShiftOperator(binary->op);
  this->adaptLiterals(binary);
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  binary->type = Type::error();
  if (leftType->isError() || rightType->isError()) {
    return ok(binary);
  }
  auto problem = this->bitsProblem(binary);
  if (problem.has_value()) {
    auto message = this->bitsProblemText(binary, *problem);

    return err(BinaryCheckerCheckBitsError{
        .code = *problem,
        .message = message,
    });
  }
  if (isShift) {
    binary->rhs = this->typeRelations->convert(binary->rhs, leftType);
    binary->type = leftType;
    this->constantFolder->foldBinary(binary);
    return ok(binary);
  }
  if (leftType->isWrapping() || rightType->isWrapping()) {
    return this->checkWrappingBits(binary);
  }
  if (leftType != rightType) {
    auto message = this->mixedNumbersText(binary);

    return err(BinaryCheckerCheckBitsError{
        .code = BinaryCheckerCheckBitsErrorCode::MIXED_NUMBERS,
        .message = message,
    });
  }
  binary->type = leftType;
  this->constantFolder->foldBinary(binary);

  return ok(binary);
}

std::optional<BinaryCheckerCheckBitsErrorCode>
BinaryChecker::bitsProblem(const BinaryExpr *binary) const {
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  auto *zero = dyn_cast<IntLitExpr>(binary->rhs);
  bool isOrZero = leftType->isNumeric() && !leftType->isWrapping() && zero != nullptr &&
                  zero->value == 0 &&
                  (binary->op == BinaryOp::BitOr || binary->op == BinaryOp::UShr);
  if (isOrZero) {
    return BinaryCheckerCheckBitsErrorCode::NOTHING_CHANGES;
  }
  if (leftType->isFloat() || rightType->isFloat()) {
    return BinaryCheckerCheckBitsErrorCode::DOUBLE_OPERAND;
  }
  if (!leftType->isInteger() || !rightType->isInteger()) {
    return BinaryCheckerCheckBitsErrorCode::OPERAND_TYPES;
  }
  if (binary->op == BinaryOp::UShr && !leftType->isUnsigned()) {
    return BinaryCheckerCheckBitsErrorCode::SIGNED_SHIFT;
  }

  return std::nullopt;
}

std::string BinaryChecker::bitsProblemText(const BinaryExpr *binary,
                                           BinaryCheckerCheckBitsErrorCode problem) const {
  const char *op = Operators::opSpelling(binary->op);
  switch (problem) {
    case BinaryCheckerCheckBitsErrorCode::NOTHING_CHANGES:
      return this->nothingChangesText(binary);
    case BinaryCheckerCheckBitsErrorCode::DOUBLE_OPERAND:
      return this->doubleBitsText(binary);
    case BinaryCheckerCheckBitsErrorCode::OPERAND_TYPES:
      return this->operandTypesText(op, binary->lhs->type, binary->rhs->type);
    case BinaryCheckerCheckBitsErrorCode::SIGNED_SHIFT:
      return this->signedShiftText(binary);
    case BinaryCheckerCheckBitsErrorCode::MIXED_NUMBERS:
    case BinaryCheckerCheckBitsErrorCode::DIVISOR_ZERO:
      return this->mixedNumbersText(binary);
  }

  return this->mixedNumbersText(binary);
}

std::string BinaryChecker::nothingChangesText(const BinaryExpr *binary) const {
  const char *op = Operators::opSpelling(binary->op);
  auto [lhsText, rhsText] = this->decimalChecker->sidesOf(binary);
  llvm::StringRef lhsName = lhsText.empty() ? kValuePlaceholder : lhsText;
  auto hint = this->nothingChangesHint(binary, lhsText);

  return ErrorTextNoOpBits::nothing(lhsName, op, hint);
}

std::string BinaryChecker::nothingChangesHint(const BinaryExpr *binary,
                                              llvm::StringRef lhsText) const {
  const Type *leftType = binary->lhs->type;
  if (leftType->isFloat() && this->isRoundingCall(binary->lhs)) {
    return ErrorTextNoOpBits::toInt(lhsText);
  }
  if (leftType->isFloat()) {
    return ErrorTextNoOpBits::trunc(lhsText);
  }
  auto text = this->shownOperand(lhsText);
  bool isUnsignedShift = binary->op == BinaryOp::UShr;

  return ErrorTextNoOpBits::wrap(text, isUnsignedShift);
}

bool BinaryChecker::isRoundingCall(const Expr *expr) const {
  auto *call = dyn_cast<CallExpr>(expr);
  if (call == nullptr) {
    return false;
  }

  return BuiltinGroups::isRoundingBuiltin(call->builtin());
}

std::string BinaryChecker::shownOperand(llvm::StringRef text) const {
  if (text.empty()) {
    return kValuePlaceholder;
  }

  return TypeRelations::operandText(text);
}

std::string BinaryChecker::doubleBitsText(const BinaryExpr *binary) const {
  const char *op = Operators::opSpelling(binary->op);
  auto [lhsText, rhsText] = this->decimalChecker->sidesOf(binary);
  llvm::StringRef fractionText = binary->lhs->type->isFloat() ? lhsText : rhsText;
  auto side = this->shownOperand(fractionText);

  return ErrorTextBitsDouble::fraction(op, side);
}

std::string BinaryChecker::signedShiftText(const BinaryExpr *binary) const {
  const Type *leftType = binary->lhs->type;
  auto [lhsText, rhsText] = this->decimalChecker->sidesOf(binary);
  auto text = this->shownOperand(lhsText);
  llvm::StringRef count = rhsText.empty() ? kCountPlaceholder : rhsText;
  const auto *unsignedType = Type::unsignedInteger(leftType->bitWidth());
  auto method = this->typeRelations->conversionMethod(unsignedType);

  return ErrorTextShift::isSigned(leftType->name(), text, method, count);
}

BinaryChecker::ResultBitwiseResolved BinaryChecker::checkWrappingBits(BinaryExpr *binary) {
  auto wrapped = this->checkWrapping(binary);
  if (wrapped.isOk()) {
    return ok(wrapped.value());
  }
  auto code = this->wrappingBitsCode(wrapped.error().code);

  return err(BinaryCheckerCheckBitsError{
      .code = code,
      .message = wrapped.error().message,
  });
}

BinaryCheckerCheckBitsErrorCode
BinaryChecker::wrappingBitsCode(BinaryCheckerCheckWrappingErrorCode code) const {
  switch (code) {
    case BinaryCheckerCheckWrappingErrorCode::MIXED_NUMBERS:
      return BinaryCheckerCheckBitsErrorCode::MIXED_NUMBERS;
    case BinaryCheckerCheckWrappingErrorCode::DIVISOR_ZERO:
      return BinaryCheckerCheckBitsErrorCode::DIVISOR_ZERO;
  }

  return BinaryCheckerCheckBitsErrorCode::MIXED_NUMBERS;
}

void BinaryChecker::adaptLiterals(BinaryExpr *binary) {
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  if (leftType == nullptr || rightType == nullptr || leftType == rightType || leftType->isError() ||
      rightType->isError()) {
    return;
  }
  bool isBitwise = this->isBitwiseOperator(binary->op) || this->isShiftOperator(binary->op);
  bool hasNumbers = this->isNumberLike(leftType) && this->isNumberLike(rightType);
  bool hasIntegers = leftType->isInteger() && rightType->isInteger();
  if (!hasNumbers || (isBitwise && !hasIntegers)) {
    return;
  }
  bool isPower = binary->op == BinaryOp::Pow;
  bool isLeftAdapting = this->typeRelations->isLiteralAdapting(binary->lhs, rightType) &&
                        !(isPower && rightType->isDecimal());
  if (isLeftAdapting) {
    binary->lhs = this->typeRelations->adaptLiteral(binary->lhs, rightType);
    return;
  }
  bool isRightAdapting = this->typeRelations->isLiteralAdapting(binary->rhs, leftType) &&
                         !(isPower && (leftType->isDecimal() || leftType->isFloat()));
  if (isRightAdapting) {
    binary->rhs = this->typeRelations->adaptLiteral(binary->rhs, leftType);
  }
}

bool BinaryChecker::isNumberLike(const Type *type) const {
  return type->isNumeric() || type->isDecimal();
}

std::string BinaryChecker::mixedNumbersText(const BinaryExpr *binary) const {
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  const char *op = Operators::opSpelling(binary->op);
  auto conversion = this->mixedConversion(leftType, rightType);
  auto example = this->mixedExample(binary, conversion);

  return ErrorTextMixedNumbers::mixed(op, leftType->name(), rightType->name(), example);
}

BinaryChecker::NumberConversion BinaryChecker::mixedConversion(const Type *leftType,
                                                               const Type *rightType) const {
  if (leftType->isFloat() || rightType->isFloat()) {
    return NumberConversion{
        .target = Type::doubleTy(),
        .isLeftConverted = !leftType->isFloat(),
    };
  }
  if (leftType->holds(rightType)) {
    return NumberConversion{
        .target = leftType,
        .isLeftConverted = false,
    };
  }
  if (rightType->holds(leftType)) {
    return NumberConversion{
        .target = rightType,
        .isLeftConverted = true,
    };
  }

  return NumberConversion{
      .target = Type::intTy(),
      .isLeftConverted = !leftType->isInt(),
  };
}

std::string BinaryChecker::mixedExample(const BinaryExpr *binary,
                                        const NumberConversion &conversion) const {
  const char *op = Operators::opSpelling(binary->op);
  auto [lhs, rhs] = this->decimalChecker->sidesOf(binary);
  auto method = this->typeRelations->conversionMethod(conversion.target);
  if (lhs.empty() || rhs.empty()) {
    return ErrorTextMixedNumbers::method(method);
  }
  if (conversion.isLeftConverted) {
    auto left = this->convertedText(lhs, method);
    return ErrorTextMixedNumbers::example(left, op, rhs);
  }
  auto right = this->convertedText(rhs, method);

  return ErrorTextMixedNumbers::example(lhs, op, right);
}

std::string BinaryChecker::convertedText(llvm::StringRef operand, const std::string &method) const {
  return TypeRelations::operandText(operand) + "." + method + "()";
}

std::string BinaryChecker::quotedNames(llvm::ArrayRef<llvm::StringRef> names) const {
  auto list = std::string();
  for (size_t index = 0; index < names.size(); ++index) {
    list += this->nameSeparator(index, names.size());
    list += "\"" + names[index].str() + "\"";
  }

  return list;
}

const char *BinaryChecker::nameSeparator(size_t index, size_t count) const {
  if (index == 0) {
    return "";
  }
  if (index + 1 == count) {
    return " or ";
  }

  return ", ";
}

BinaryChecker::ResultTypeOfNameKnown BinaryChecker::checkTypeOfName(const Expr *operand,
                                                                    const StringLitExpr *text) {
  auto name = ToUtf8::toUtf8(text->value);
  if (name == kBigIntTypeOf) {
    auto what = ErrorTextTypeOfName::bigint();
    auto bigint = ErrorTextBigInt::none(what);
    this->state.getDiagnostics().error(text->loc(), bigint);

    return err(BinaryCheckerCheckTypeOfNameError{
        .code = BinaryCheckerCheckTypeOfNameErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  auto known = this->checkKnownTypeOfName(operand, text, name);
  if (known.isErr()) {
    return err(known.error());
  }

  return this->checkTypeOfNameMatches(operand, text, name);
}

BinaryChecker::ResultTypeOfNameKnown
BinaryChecker::checkKnownTypeOfName(const Expr *operand, const StringLitExpr *text,
                                    const std::string &name) const {
  if (TypeSpelling::isTypeOfName(name)) {
    return ok(text);
  }
  if (name == kNumberTypeOf) {
    auto message = this->noNumberText(operand);

    return err(BinaryCheckerCheckTypeOfNameError{
        .code = BinaryCheckerCheckTypeOfNameErrorCode::NO_NUMBER,
        .message = message,
    });
  }
  std::vector<llvm::StringRef> all(std::begin(kTypeOfNames), std::end(kTypeOfNames));
  auto list = this->quotedNames(all);
  if (name == kSymbolTypeOf) {
    auto message = ErrorTextTypeOfName::noSymbol(list);

    return err(BinaryCheckerCheckTypeOfNameError{
        .code = BinaryCheckerCheckTypeOfNameErrorCode::NO_SYMBOL,
        .message = message,
    });
  }
  auto message = ErrorTextTypeOfName::notName(name, list);

  return err(BinaryCheckerCheckTypeOfNameError{
      .code = BinaryCheckerCheckTypeOfNameErrorCode::NOT_A_NAME,
      .message = message,
  });
}

std::string BinaryChecker::noNumberText(const Expr *operand) const {
  auto here = this->numberNamesHere(operand);

  return ErrorTextTypeOfName::noNumber(here);
}

std::string BinaryChecker::numberNamesHere(const Expr *operand) const {
  auto numbers = this->numberTypeOfNames(operand->type);
  if (numbers.empty() || numbers.size() > kMostShownNumberNames) {
    return "";
  }
  auto quoted = this->quotedNames(numbers);

  return ErrorTextTypeOfName::forType(operand->type->name(), quoted);
}

std::vector<llvm::StringRef> BinaryChecker::numberTypeOfNames(const Type *type) const {
  auto numbers = std::vector<llvm::StringRef>();
  if (type == nullptr || type->isError()) {
    return numbers;
  }
  auto names = TypeSpelling::typeOfNames(type);
  for (llvm::StringRef possible : names) {
    if (TypeSpelling::isNumberTypeOfName(possible)) {
      numbers.push_back(possible);
    }
  }

  return numbers;
}

BinaryChecker::ResultTypeOfNameKnown
BinaryChecker::checkTypeOfNameMatches(const Expr *operand, const StringLitExpr *text,
                                      const std::string &name) const {
  const Type *type = operand->type;
  bool isUnchecked = type == nullptr || type->isError() ||
                     this->state.getGenericStore().isCheckingInstance() ||
                     type->nonOptional()->isParam();
  if (isUnchecked) {
    return ok(text);
  }
  auto names = TypeSpelling::typeOfNames(type);
  bool isMatching = names.empty() || llvm::is_contained(names, llvm::StringRef(name));
  if (!isMatching) {
    auto quoted = this->quotedNames(names);
    auto message = ErrorTextTypeOfName::never(type->name(), quoted, name);

    return err(BinaryCheckerCheckTypeOfNameError{
        .code = BinaryCheckerCheckTypeOfNameErrorCode::NEVER_MATCHES,
        .message = message,
    });
  }

  return ok(text);
}

BinaryChecker::ResultNoResultOperand BinaryChecker::checkNoResults(const BinaryExpr *binary) const {
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  if (leftType->nonOptional()->isResult() || rightType->nonOptional()->isResult()) {
    const char *op = Operators::opSpelling(binary->op);
    auto message = ErrorTextResultEquality::compared(op);

    return err(BinaryCheckerNoResultsError{
        .code = BinaryCheckerNoResultsErrorCode::RESULTS_COMPARED,
        .message = message,
    });
  }

  return ok(binary);
}

Expr *BinaryChecker::checkEquality(BinaryExpr *binary) {
  auto *nullCompared = this->jsonChecker->checkNullEquality(binary);
  if (nullCompared != nullptr) {
    return nullCompared;
  }
  this->decimalChecker->walkComparisonOperands(binary);
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  binary->type = Type::boolTy();
  this->walkTypeOfSide(binary->lhs, binary->rhs);
  this->walkTypeOfSide(binary->rhs, binary->lhs);
  if (leftType->isError() || rightType->isError()) {
    return binary;
  }
  if (leftType->isVoid() || rightType->isVoid()) {
    const char *op = Operators::opSpelling(binary->op);
    auto message = this->operandTypesText(op, leftType, rightType);
    this->state.getDiagnostics().error(binary->loc(), message);
    return binary;
  }
  if (leftType->isUndefined() || rightType->isUndefined()) {
    return binary;
  }
  if (leftType->nonOptional()->isJson() || rightType->nonOptional()->isJson()) {
    return this->jsonChecker->checkJsonEquality(binary);
  }
  auto plain = this->checkNoResults(binary);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(binary->loc(), plain.error().message);
    return binary;
  }
  if (leftType->nonOptional()->isParam() || rightType->nonOptional()->isParam()) {
    return binary;
  }

  return this->checkEqualOperands(binary);
}

void BinaryChecker::walkTypeOfSide(const Expr *side, const Expr *other) {
  auto *typeOf = dyn_cast<UnaryExpr>(side);
  auto *text = dyn_cast<StringLitExpr>(other);
  if (typeOf == nullptr || typeOf->op != UnaryOp::TypeOf || text == nullptr) {
    return;
  }
  auto known = this->checkTypeOfName(typeOf->operand, text);
  if (known.isErr() &&
      known.error().code != BinaryCheckerCheckTypeOfNameErrorCode::ALREADY_REPORTED) {
    this->state.getDiagnostics().error(text->loc(), known.error().message);
  }
}

Expr *BinaryChecker::checkEqualOperands(BinaryExpr *binary) {
  this->adaptLiterals(binary);
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  if (leftType->isInteger() && rightType->isInteger()) {
    return binary;
  }
  const auto *common = this->typeRelations->comparisonType(leftType, rightType);
  if (common == nullptr) {
    this->reportIncomparable(binary);
    return binary;
  }
  if (common->nonOptional()->isFunction()) {
    this->state.getInventoryStore().markComparesFunctions();
  }
  binary->lhs = this->typeRelations->convert(binary->lhs, common);
  binary->rhs = this->typeRelations->convert(binary->rhs, common);

  return binary;
}

void BinaryChecker::reportIncomparable(const BinaryExpr *binary) {
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  if (leftType->nonOptional()->isDecimal() || rightType->nonOptional()->isDecimal()) {
    return this->decimalChecker->reportDecimalMix(binary);
  }
  bool hasNumbers = leftType->nonOptional()->isNumeric() && rightType->nonOptional()->isNumeric() &&
                    !leftType->isOptional() && !rightType->isOptional();
  const char *op = Operators::opSpelling(binary->op);
  std::string message =
      hasNumbers ? this->mixedNumbersText(binary) : this->operandTypesText(op, leftType, rightType);

  return this->state.getDiagnostics().error(binary->loc(), message);
}

BinaryChecker::ResultNullishResolved BinaryChecker::checkNullish(BinaryExpr *binary,
                                                                 const Type *expected) {
  this->walkNullishOperands(binary, expected);
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  binary->type = Type::error();
  if (leftType->isError() || rightType->isError()) {
    binary->type = this->failedNullishType(leftType, rightType);
    return ok(binary);
  }
  if (leftType->isVoid() || rightType->isVoid()) {
    auto message = this->operandTypesText("??", leftType, rightType);

    return err(BinaryCheckerCheckNullishError{
        .code = BinaryCheckerCheckNullishErrorCode::VOID_OPERAND,
        .message = message,
    });
  }
  if (leftType->isUndefined()) {
    binary->type = rightType;
    return ok(binary);
  }
  const Type *base = rightType->isUndefined() ? leftType->nonOptional()
                                              : LiteralTypes::unionUnify(leftType->nonOptional(),
                                                                         rightType->nonOptional());
  if (base == nullptr || base->isError()) {
    auto message = this->operandTypesText("??", leftType, rightType);

    return err(BinaryCheckerCheckNullishError{
        .code = BinaryCheckerCheckNullishErrorCode::NO_COMMON_TYPE,
        .message = message,
    });
  }
  const Type *result =
      rightType->isOptional() || rightType->isUndefined() ? Type::optional(base) : base;
  const Type *leftTarget = leftType->isOptional() ? Type::optional(base) : result;
  binary->lhs = this->typeRelations->convert(binary->lhs, leftTarget);
  binary->rhs = this->typeRelations->convert(binary->rhs, result);
  binary->type = result;

  return ok(binary);
}

void BinaryChecker::walkNullishOperands(BinaryExpr *binary, const Type *expected) {
  binary->lhs = this->exprChecker->checkExpr(binary->lhs, expected);
  if (binary->lhs->type->nonOptional()->isMatch() && isa<ArrayLitExpr>(binary->rhs)) {
    binary->lhs = this->matchChecker->matchItems(binary->lhs);
  }
  const Type *leftType = binary->lhs->type;
  auto afterLhs = this->state.getFlowState().getFlow();
  const Type *hint =
      leftType->isUndefined() || leftType->isError() ? expected : leftType->nonOptional();
  binary->rhs = this->exprChecker->checkExpr(binary->rhs, hint);
  const Flow &afterRhs = this->state.getFlowState().getFlow();
  auto joined = this->flowChecker->join(afterLhs, afterRhs);
  this->state.getFlowState().replaceFlow(std::move(joined));
}

const Type *BinaryChecker::failedNullishType(const Type *leftType, const Type *rightType) const {
  bool isOneSideKnown =
      this->state.getFunctionStack().isInsideTrial() && leftType->isError() != rightType->isError();
  if (!isOneSideKnown) {
    return Type::error();
  }

  return leftType->isError() ? rightType : leftType->nonOptional();
}

Expr *BinaryChecker::toInt32(Expr *expr) {
  return expr->type->isInt() || expr->type->isError()
             ? expr
             : this->typeRelations->convert(expr, Type::intTy());
}

}
