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

BinaryChecker::BinaryChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->dateChecker = &all.getDateChecker();
  this->exprChecker = &all.getExprChecker();
  this->jsonChecker = &all.getJsonChecker();
  this->resultTypes = &all.getResultTypes();
  this->matchChecker = &all.getMatchChecker();
  this->truthChecker = &all.getTruthChecker();
  this->unaryChecker = &all.getUnaryChecker();
  this->concatChecker = &all.getConcatChecker();
  this->typeRelations = &all.getTypeRelations();
  this->constantFolder = &all.getConstantFolder();
  this->decimalChecker = &all.getDecimalChecker();
  this->genericChecker = &all.getGenericChecker();
  this->instanceOfChecker = &all.getInstanceOfChecker();
}

bool BinaryChecker::onlyIntLiterals(const Expr *expr) {
  for (const Expr *node : ExprParts::operatorTree(expr)) {
    if (!BinaryChecker::isIntLiteralNode(node)) {
      return false;
    }
  }

  return true;
}

bool BinaryChecker::isIntLiteralNode(const Expr *expr) {
  if (isa<IntLitExpr>(expr)) {
    return true;
  }
  if (auto *unary = dyn_cast<UnaryExpr>(expr)) {
    return unary->op == UnaryOp::Neg || unary->op == UnaryOp::Plus;
  }
  if (auto *bin = dyn_cast<BinaryExpr>(expr)) {
    return bin->op == BinaryOp::Add || bin->op == BinaryOp::Sub || bin->op == BinaryOp::Mul ||
           bin->op == BinaryOp::Rem || bin->op == BinaryOp::Pow;
  }

  return false;
}

const Type *BinaryChecker::integerHint(const Expr *literal, const Expr *other, const Type *hint,
                                       bool bitwise) {
  const Type *type = other->type ? other->type->nonOptional() : nullptr;
  if (type && (type->isInteger() || (type->isFloat() && !bitwise)) &&
      BinaryChecker::onlyIntLiterals(literal) && !TypeRelations::isIntLiteralTree(other)) {
    return type;
  }

  return hint;
}

std::string BinaryChecker::operandTypesText(llvm::StringRef op, const Type *lhs, const Type *rhs) {
  bool undefined =
      lhs->isOptional() || rhs->isOptional() || lhs->isUndefined() || rhs->isUndefined();

  return ErrorTextOperands::types(op, lhs->name(), rhs->name(), undefined);
}

BinaryChecker::ResultExponentIsValid BinaryChecker::powerExponent(const Expr *rhs,
                                                                  const Type *common) const {
  std::optional<int64_t> exponent = this->constantFolder->intConstant(rhs);
  if (exponent && *exponent < 0) {
    std::string message = ErrorTextPower::negative(common->name());

    return err(BinaryCheckerPowerExponentError{
        .code = BinaryCheckerPowerExponentErrorCode::NEGATIVE_POWER,
        .message = message,
    });
  }

  return ok(rhs);
}

Expr *BinaryChecker::checkBinary(BinaryExpr *bin, const Type *expected, bool wraps) {
  const Type *expectedValue = expected != nullptr ? expected->nonOptional() : nullptr;
  switch (bin->op) {
    case BinaryOp::In:
      return this->checkInOperator(bin);
    case BinaryOp::InstanceOf:
      return this->checkInstanceOfOperator(bin);
    case BinaryOp::LogAnd:
    case BinaryOp::LogOr:
      return this->checkLogicalOperator(bin, expectedValue);
    case BinaryOp::Nullish:
      return this->checkNullishOperator(bin, expectedValue);
    case BinaryOp::Eq:
    case BinaryOp::Ne:
      return this->checkEquality(bin);
    case BinaryOp::Lt:
    case BinaryOp::Le:
    case BinaryOp::Gt:
    case BinaryOp::Ge:
      return this->checkComparison(bin);
    case BinaryOp::Shl:
    case BinaryOp::Shr:
    case BinaryOp::UShr:
      return this->checkShift(bin, expectedValue);
    case BinaryOp::Add:
    case BinaryOp::Sub:
    case BinaryOp::Mul:
    case BinaryOp::Div:
    case BinaryOp::Rem:
    case BinaryOp::Pow:
    case BinaryOp::BitAnd:
    case BinaryOp::BitOr:
    case BinaryOp::BitXor:
      return this->checkArithmetic(bin, expectedValue, wraps);
  }

  return bin;
}

Expr *BinaryChecker::checkInOperator(BinaryExpr *bin) {
  UnaryChecker::ResultInOperatorSuited in = this->unaryChecker->checkIn(bin);
  if (in.isErr()) {
    llvm::SMLoc at = BinaryChecker::inErrorPlace(bin, in.error().code);
    this->state.getDiagnostics().error(at, in.error().message);
    return bin;
  }

  return in.value();
}

llvm::SMLoc BinaryChecker::inErrorPlace(const BinaryExpr *bin, UnaryCheckerCheckInErrorCode code) {
  switch (code) {
    case UnaryCheckerCheckInErrorCode::DICT_UNDEFINED:
    case UnaryCheckerCheckInErrorCode::UNION_UNDEFINED:
      return bin->rhs->loc();
    case UnaryCheckerCheckInErrorCode::DICT_NUMBER_KEY:
      return bin->lhs->loc();
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
      return bin->loc();
  }

  return bin->loc();
}

Expr *BinaryChecker::checkInstanceOfOperator(BinaryExpr *bin) {
  InstanceOfChecker::ResultInstanceTested tested = this->instanceOfChecker->checkInstanceOf(bin);
  if (tested.isErr()) {
    llvm::SMLoc at = BinaryChecker::instanceOfErrorPlace(bin, tested.error().code);
    this->state.getDiagnostics().error(at, tested.error().message);
    return bin;
  }

  return tested.value();
}

llvm::SMLoc BinaryChecker::instanceOfErrorPlace(const BinaryExpr *bin,
                                                InstanceOfCheckerCheckInstanceOfErrorCode code) {
  switch (code) {
    case InstanceOfCheckerCheckInstanceOfErrorCode::NOT_AN_OBJECT:
      return bin->lhs->loc();
    case InstanceOfCheckerCheckInstanceOfErrorCode::NOT_A_CLASS:
      return bin->rhs->loc();
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
      return bin->loc();
  }

  return bin->loc();
}

Expr *BinaryChecker::checkLogicalOperator(BinaryExpr *bin, const Type *expected) {
  Flow whenTrue;
  Flow whenFalse;
  TruthUse use = bin->op == BinaryOp::LogAnd ? TruthUse::And : TruthUse::Or;
  this->truthChecker->checkLogical(bin, expected, whenTrue, whenFalse, false, use);
  Flow joined = FlowChecker::join(whenTrue, whenFalse);
  this->state.getFlowState().replaceFlow(std::move(joined));

  return bin;
}

Expr *BinaryChecker::checkNullishOperator(BinaryExpr *bin, const Type *expected) {
  ResultNullishResolved nullish = this->checkNullish(bin, expected);
  if (nullish.isErr()) {
    this->state.getDiagnostics().error(bin->loc(), nullish.error().message);
    return bin;
  }

  return nullish.value();
}

Expr *BinaryChecker::checkComparison(BinaryExpr *bin) {
  this->decimalChecker->checkComparisonOperands(bin);
  this->resultTypes->neverOperands(bin);
  bin->type = Type::boolTy();
  bool sameParam = bin->lhs->type->isParam() && bin->lhs->type == bin->rhs->type;
  if (sameParam) {
    return bin;
  }

  bin->lhs = this->genericChecker->apparent(bin->lhs);
  bin->rhs = this->genericChecker->apparent(bin->rhs);
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
  if (l->isError() || r->isError()) {
    return bin;
  }
  if (l->isString() && r->isString()) {
    return bin;
  }
  Expr *dates = this->dateChecker->checkDateOperands(bin);
  if (dates != nullptr) {
    return dates;
  }
  if (l->isDecimal() || r->isDecimal()) {
    return this->decimalChecker->checkDecimalComparison(bin);
  }
  if (l->isNumeric() && r->isNumeric()) {
    return this->checkNumberComparison(bin);
  }
  const char *op = Operators::opSpelling(bin->op);
  std::string message = BinaryChecker::operandTypesText(op, l, r);
  this->state.getDiagnostics().error(bin->loc(), message);

  return bin;
}

Expr *BinaryChecker::checkNumberComparison(BinaryExpr *bin) {
  this->adaptLiterals(bin);
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
  bool comparable = l == r || (l->isInteger() && r->isInteger());
  if (!comparable) {
    std::string message = BinaryChecker::mixedNumbersText(bin);
    this->state.getDiagnostics().error(bin->loc(), message);
  }

  return bin;
}

Expr *BinaryChecker::checkShift(BinaryExpr *bin, const Type *expected) {
  bool integerExpected = expected != nullptr && expected->isInteger();
  const Type *hint = integerExpected ? expected : nullptr;
  const Type *lhsHint = BinaryChecker::onlyIntLiterals(bin->lhs) ? hint : nullptr;
  Expr *lhs = this->exprChecker->checkExpr(bin->lhs, lhsHint);
  bin->lhs = this->genericChecker->apparent(lhs);
  Expr *rhs = this->exprChecker->checkExpr(bin->rhs);
  bin->rhs = this->genericChecker->apparent(rhs);

  return this->checkBitwiseOperator(bin);
}

Expr *BinaryChecker::checkBitwiseOperator(BinaryExpr *bin) {
  ResultBitwiseResolved bits = this->checkBits(bin);
  if (bits.isErr()) {
    bool divisor = bits.error().code == BinaryCheckerCheckBitsErrorCode::DIVISOR_ZERO;
    llvm::SMLoc at = divisor ? bin->rhs->loc() : bin->loc();
    this->state.getDiagnostics().error(at, bits.error().message);
    return bin;
  }

  return bits.value();
}

bool BinaryChecker::isBitwise(BinaryOp op) {
  return op == BinaryOp::BitAnd || op == BinaryOp::BitOr || op == BinaryOp::BitXor;
}

Expr *BinaryChecker::checkArithmetic(BinaryExpr *bin, const Type *expected, bool wraps) {
  ExpressionMarks &marks = this->state.getExpressionMarks();
  BinaryExpr *chain = marks.takeConcatChain();
  bool chainEnd = bin->op == BinaryOp::Add && chain == nullptr;
  const BinaryExpr *outerMixed = chainEnd ? marks.takeMixedConcat() : nullptr;
  auto *left = dyn_cast<BinaryExpr>(bin->lhs);
  bool chainGoesLeft = bin->op == BinaryOp::Add && left != nullptr && left->op == BinaryOp::Add;
  if (chainGoesLeft) {
    BinaryExpr *chainStart = chain != nullptr ? chain : bin;
    marks.rememberConcatChain(chainStart);
  }

  this->walkArithmeticOperands(bin, expected);
  this->resultTypes->neverOperands(bin);
  this->adaptLiterals(bin);
  if (bin->op == BinaryOp::Add) {
    this->trackMixedConcat(bin, chainEnd, outerMixed);
  }

  return this->checkArithmeticResult(bin, wraps);
}

BinaryChecker::OperandHints BinaryChecker::operandHints(const BinaryExpr *bin,
                                                        const Type *expected) {
  bool bitwise = BinaryChecker::isBitwise(bin->op);
  bool power = bin->op == BinaryOp::Pow;
  bool numberExpected =
      expected != nullptr && !bitwise && (expected->isNumeric() || expected->isDecimal());
  const Type *hint = numberExpected ? expected : nullptr;
  bool decimalPower = power && hint != nullptr && hint->isDecimal();

  return OperandHints{
      .hint = hint,
      .rhsHint = decimalPower ? nullptr : hint,
      .bitwise = bitwise,
      .literals = !bitwise && !power,
  };
}

void BinaryChecker::walkArithmeticOperands(BinaryExpr *bin, const Type *expected) {
  OperandHints hints = BinaryChecker::operandHints(bin, expected);
  bool literalLeft = DecimalChecker::isLiteralTree(bin->lhs) &&
                     !DecimalChecker::isLiteralTree(bin->rhs) &&
                     (hints.literals || BinaryChecker::onlyIntLiterals(bin->lhs));
  if (literalLeft) {
    return this->walkRightOperandFirst(bin, hints);
  }

  return this->walkLeftOperandFirst(bin, hints);
}

void BinaryChecker::walkRightOperandFirst(BinaryExpr *bin, const OperandHints &hints) {
  ExpressionMarks &marks = this->state.getExpressionMarks();
  BinaryExpr *pending = marks.takeConcatChain();
  Expr *rhs = this->exprChecker->checkExpr(bin->rhs, hints.rhsHint);
  bin->rhs = this->genericChecker->apparent(rhs);
  marks.rememberConcatChain(pending);
  const Type *literalHint =
      hints.literals ? DecimalChecker::literalHint(bin->rhs, hints.hint) : hints.hint;
  const Type *lhsHint = BinaryChecker::integerHint(bin->lhs, bin->rhs, literalHint, hints.bitwise);
  Expr *lhs = this->exprChecker->checkExpr(bin->lhs, lhsHint);
  bin->lhs = this->genericChecker->apparent(lhs);
  marks.forgetConcatChain();
}

void BinaryChecker::walkLeftOperandFirst(BinaryExpr *bin, const OperandHints &hints) {
  ExpressionMarks &marks = this->state.getExpressionMarks();
  Expr *lhs = this->exprChecker->checkExpr(bin->lhs, hints.hint);
  bin->lhs = this->genericChecker->apparent(lhs);
  marks.forgetConcatChain();
  const Type *literalHint = hints.rhsHint;
  bool literalRight = hints.literals && DecimalChecker::isLiteralTree(bin->rhs);
  if (literalRight) {
    literalHint = DecimalChecker::literalHint(bin->lhs, hints.rhsHint);
  }
  const Type *rhsHint = BinaryChecker::integerHint(bin->rhs, bin->lhs, literalHint, hints.bitwise);
  Expr *rhs = this->exprChecker->checkExpr(bin->rhs, rhsHint);
  bin->rhs = this->genericChecker->apparent(rhs);
}

void BinaryChecker::trackMixedConcat(BinaryExpr *bin, bool chainEnd, const BinaryExpr *outerMixed) {
  ExpressionMarks &marks = this->state.getExpressionMarks();
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
  bool mixes = (l->isString() && BinaryChecker::joinsAsText(r)) ||
               (r->isString() && BinaryChecker::joinsAsText(l));
  if (marks.getMixedConcat() == nullptr && mixes) {
    marks.rememberMixedConcat(bin);
  }
  if (chainEnd) {
    const BinaryExpr *first = marks.getMixedConcat();
    if (first != nullptr) {
      std::string mixed = ConcatChecker::mixedConcatText(bin, first);
      this->state.getDiagnostics().error(first->loc(), mixed);
    }
    marks.rememberMixedConcat(outerMixed);
  }
}

bool BinaryChecker::joinsAsText(const Type *type) {
  const Type *base = type->nonOptional();

  return base->isNumeric() || base->isDecimal() || base->isBool();
}

Expr *BinaryChecker::checkArithmeticResult(BinaryExpr *bin, bool wraps) {
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
  bin->type = Type::error();
  if (l->isError() || r->isError()) {
    return bin;
  }
  bool concatenates = bin->op == BinaryOp::Add && (l->isString() || r->isString());
  if (concatenates) {
    return this->checkConcatenation(bin);
  }
  Expr *dates = this->dateChecker->checkDateOperands(bin);
  if (dates != nullptr) {
    return dates;
  }
  if (BinaryChecker::isBitwise(bin->op)) {
    return this->checkBitwiseOperator(bin);
  }
  if (l->isDecimal() || r->isDecimal()) {
    return this->decimalChecker->checkDecimalArithmetic(bin);
  }
  bool wrapping = (l->isWrapping() || r->isWrapping()) && l->isInteger() && r->isInteger();
  if (wrapping) {
    return this->checkWrappingArithmetic(bin);
  }

  return this->checkNumberArithmetic(bin, wraps);
}

Expr *BinaryChecker::checkConcatenation(BinaryExpr *bin) {
  bin->lhs = this->concatChecker->toStringOperand(bin->lhs);
  bin->rhs = this->concatChecker->toStringOperand(bin->rhs);
  bin->type = Type::string();

  return bin;
}

Expr *BinaryChecker::checkWrappingArithmetic(BinaryExpr *bin) {
  ResultWrappingChecked wrapped = this->checkWrapping(bin);
  if (wrapped.isErr()) {
    bool divisor = wrapped.error().code == BinaryCheckerCheckWrappingErrorCode::DIVISOR_ZERO;
    llvm::SMLoc at = divisor ? bin->rhs->loc() : bin->loc();
    this->state.getDiagnostics().error(at, wrapped.error().message);
    return bin;
  }

  return wrapped.value();
}

Expr *BinaryChecker::checkNumberArithmetic(BinaryExpr *bin, bool wraps) {
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
  bool floatPower = bin->op == BinaryOp::Pow && l->isFloat() && r->isInteger();
  if (floatPower) {
    bin->rhs = this->typeRelations->convert(bin->rhs, l);
    r = l;
  }
  bool numbers = l->isNumeric() && r->isNumeric();
  const Type *common = numbers ? TypeRelations::commonNumericType(l, r) : nullptr;
  if (numbers && common == nullptr) {
    std::string message = BinaryChecker::mixedNumbersText(bin);
    this->state.getDiagnostics().error(bin->loc(), message);
    return bin;
  }
  if (common == nullptr) {
    const char *op = Operators::opSpelling(bin->op);
    std::string message = BinaryChecker::operandTypesText(op, l, r);
    this->state.getDiagnostics().error(bin->loc(), message);
    return bin;
  }

  bin->lhs = this->typeRelations->convert(bin->lhs, common);
  bin->rhs = this->typeRelations->convert(bin->rhs, common);
  bin->type = common;
  if (!common->isInteger()) {
    return bin;
  }

  return this->checkIntegerArithmetic(bin, common, wraps);
}

Expr *BinaryChecker::checkIntegerArithmetic(BinaryExpr *bin, const Type *common, bool wraps) {
  if (bin->op == BinaryOp::Pow) {
    ResultExponentIsValid power = this->powerExponent(bin->rhs, common);
    if (power.isErr()) {
      this->state.getDiagnostics().error(bin->rhs->loc(), power.error().message);
      return bin;
    }
  }
  ConstantFolder::ResultDivisorNotZero divisor =
      this->constantFolder->checkDivisor(bin->op, bin->rhs);
  if (divisor.isErr()) {
    this->state.getDiagnostics().error(bin->rhs->loc(), divisor.error().message);
  }
  this->constantFolder->foldBinary(bin, wraps);

  return bin;
}

BinaryChecker::ResultWrappingChecked BinaryChecker::checkWrapping(BinaryExpr *bin) {
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
  if (l->isWrapping() && r->isWrapping() && l != r) {
    std::string message = BinaryChecker::mixedNumbersText(bin);

    return err(BinaryCheckerCheckWrappingError{
        .code = BinaryCheckerCheckWrappingErrorCode::MIXED_NUMBERS,
        .message = message,
    });
  }
  const Type *wrap = l->isWrapping() ? l : r;
  bin->lhs = this->typeRelations->convert(bin->lhs, wrap);
  bin->rhs = this->typeRelations->convert(bin->rhs, wrap);
  bin->type = wrap;
  ConstantFolder::ResultDivisorNotZero divisor =
      this->constantFolder->checkDivisor(bin->op, bin->rhs);
  if (divisor.isErr()) {
    return err(BinaryCheckerCheckWrappingError{
        .code = BinaryCheckerCheckWrappingErrorCode::DIVISOR_ZERO,
        .message = divisor.error().message,
    });
  }

  return ok(bin);
}

BinaryChecker::ResultBitwiseResolved BinaryChecker::checkBits(BinaryExpr *bin) {
  bool shift = bin->op == BinaryOp::Shl || bin->op == BinaryOp::Shr || bin->op == BinaryOp::UShr;
  this->adaptLiterals(bin);
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
  bin->type = Type::error();
  if (l->isError() || r->isError()) {
    return ok(bin);
  }
  std::optional<BinaryCheckerCheckBitsErrorCode> problem = BinaryChecker::bitsProblem(bin, shift);
  if (problem.has_value()) {
    std::string message = BinaryChecker::bitsProblemText(bin, *problem);

    return err(BinaryCheckerCheckBitsError{
        .code = *problem,
        .message = message,
    });
  }
  if (shift) {
    bin->rhs = this->typeRelations->convert(bin->rhs, l);
    bin->type = l;
    this->constantFolder->foldBinary(bin, false);
    return ok(bin);
  }
  if (l->isWrapping() || r->isWrapping()) {
    return this->checkWrappingBits(bin);
  }
  if (l != r) {
    std::string message = BinaryChecker::mixedNumbersText(bin);

    return err(BinaryCheckerCheckBitsError{
        .code = BinaryCheckerCheckBitsErrorCode::MIXED_NUMBERS,
        .message = message,
    });
  }
  bin->type = l;
  this->constantFolder->foldBinary(bin, false);

  return ok(bin);
}

std::optional<BinaryCheckerCheckBitsErrorCode> BinaryChecker::bitsProblem(const BinaryExpr *bin,
                                                                          bool shift) {
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
  auto *zero = dyn_cast<IntLitExpr>(bin->rhs);
  bool orZero = l->isNumeric() && !l->isWrapping() && zero != nullptr && zero->value == 0 &&
                (bin->op == BinaryOp::BitOr || bin->op == BinaryOp::UShr);
  if (orZero) {
    return BinaryCheckerCheckBitsErrorCode::NOTHING_CHANGES;
  }
  if (l->isFloat() || r->isFloat()) {
    return BinaryCheckerCheckBitsErrorCode::DOUBLE_OPERAND;
  }
  if (!l->isInteger() || !r->isInteger()) {
    return BinaryCheckerCheckBitsErrorCode::OPERAND_TYPES;
  }
  if (shift && bin->op == BinaryOp::UShr && !l->isUnsigned()) {
    return BinaryCheckerCheckBitsErrorCode::SIGNED_SHIFT;
  }

  return std::nullopt;
}

std::string BinaryChecker::bitsProblemText(const BinaryExpr *bin,
                                           BinaryCheckerCheckBitsErrorCode problem) {
  const char *op = Operators::opSpelling(bin->op);
  switch (problem) {
    case BinaryCheckerCheckBitsErrorCode::NOTHING_CHANGES:
      return BinaryChecker::nothingChangesText(bin);
    case BinaryCheckerCheckBitsErrorCode::DOUBLE_OPERAND:
      return BinaryChecker::doubleBitsText(bin);
    case BinaryCheckerCheckBitsErrorCode::OPERAND_TYPES:
      return BinaryChecker::operandTypesText(op, bin->lhs->type, bin->rhs->type);
    case BinaryCheckerCheckBitsErrorCode::SIGNED_SHIFT:
      return BinaryChecker::signedShiftText(bin);
    case BinaryCheckerCheckBitsErrorCode::MIXED_NUMBERS:
    case BinaryCheckerCheckBitsErrorCode::DIVISOR_ZERO:
      return BinaryChecker::mixedNumbersText(bin);
  }

  return BinaryChecker::mixedNumbersText(bin);
}

std::string BinaryChecker::nothingChangesText(const BinaryExpr *bin) {
  const Type *l = bin->lhs->type;
  const char *op = Operators::opSpelling(bin->op);
  auto [lhsText, rhsText] = DecimalChecker::sidesOf(bin);
  std::string text = lhsText.empty() ? "x" : TypeRelations::operandText(lhsText);
  std::string lhsName = lhsText.empty() ? "x" : lhsText.str();
  auto *rounded = dyn_cast<CallExpr>(bin->lhs);
  bool roundedFloat =
      l->isFloat() && rounded != nullptr && BuiltinGroups::isRoundingBuiltin(rounded->builtin());
  std::string hint = roundedFloat   ? ErrorTextNoOpBits::toInt(lhsText)
                     : l->isFloat() ? ErrorTextNoOpBits::trunc(lhsText)
                                    : ErrorTextNoOpBits::wrap(text, bin->op == BinaryOp::UShr);

  return ErrorTextNoOpBits::nothing(lhsName, op, hint);
}

std::string BinaryChecker::doubleBitsText(const BinaryExpr *bin) {
  const char *op = Operators::opSpelling(bin->op);
  auto [lhsText, rhsText] = DecimalChecker::sidesOf(bin);
  std::string text = lhsText.empty() ? "x" : TypeRelations::operandText(lhsText);
  std::string side = bin->lhs->type->isFloat() ? text
                     : rhsText.empty()         ? "x"
                                               : TypeRelations::operandText(rhsText);

  return ErrorTextBitsDouble::fraction(op, side);
}

std::string BinaryChecker::signedShiftText(const BinaryExpr *bin) {
  const Type *l = bin->lhs->type;
  auto [lhsText, rhsText] = DecimalChecker::sidesOf(bin);
  std::string text = lhsText.empty() ? "x" : TypeRelations::operandText(lhsText);
  std::string count = rhsText.empty() ? "n" : rhsText.str();
  const Type *unsignedType = Type::integer(l->bitWidth(), true);
  std::string method = TypeRelations::conversionMethod(unsignedType);

  return ErrorTextShift::isSigned(l->name(), text, method, count);
}

BinaryChecker::ResultBitwiseResolved BinaryChecker::checkWrappingBits(BinaryExpr *bin) {
  ResultWrappingChecked wrapped = this->checkWrapping(bin);
  if (wrapped.isOk()) {
    return ok(wrapped.value());
  }
  bool divisor = wrapped.error().code == BinaryCheckerCheckWrappingErrorCode::DIVISOR_ZERO;

  return err(BinaryCheckerCheckBitsError{
      .code = divisor ? BinaryCheckerCheckBitsErrorCode::DIVISOR_ZERO
                      : BinaryCheckerCheckBitsErrorCode::MIXED_NUMBERS,
      .message = wrapped.error().message,
  });
}

void BinaryChecker::adaptLiterals(BinaryExpr *bin) {
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
  if (!l || !r || l == r || l->isError() || r->isError()) {
    return;
  }
  auto number = [](const Type *t) { return t->isNumeric() || t->isDecimal(); };
  bool bitwise = bin->op == BinaryOp::BitAnd || bin->op == BinaryOp::BitOr ||
                 bin->op == BinaryOp::BitXor || bin->op == BinaryOp::Shl ||
                 bin->op == BinaryOp::Shr || bin->op == BinaryOp::UShr;
  if (!number(l) || !number(r) || (bitwise && (!l->isInteger() || !r->isInteger()))) {
    return;
  }
  if (this->typeRelations->literalAdapts(bin->lhs, r) &&
      !(bin->op == BinaryOp::Pow && r->isDecimal())) {
    bin->lhs = this->typeRelations->adaptLiteral(bin->lhs, r);
  } else if (this->typeRelations->literalAdapts(bin->rhs, l) &&
             !(bin->op == BinaryOp::Pow && (l->isDecimal() || l->isFloat()))) {
    bin->rhs = this->typeRelations->adaptLiteral(bin->rhs, l);
  }
}

std::string BinaryChecker::mixedNumbersText(const BinaryExpr *bin) {
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
  const char *op = Operators::opSpelling(bin->op);
  const Type *target = nullptr;
  bool convertLeft = false;
  if (l->isFloat() || r->isFloat()) {
    target = Type::doubleTy();
    convertLeft = !l->isFloat();
  } else if (l->holds(r)) {
    target = l;
  } else if (r->holds(l)) {
    target = r;
    convertLeft = true;
  } else {
    target = Type::intTy();
    convertLeft = !l->isInt();
  }
  auto [lhs, rhs] = DecimalChecker::sidesOf(bin);
  std::string method = TypeRelations::conversionMethod(target);
  std::string example;
  if (!lhs.empty() && !rhs.empty()) {
    std::string left =
        convertLeft ? TypeRelations::operandText(lhs) + "." + method + "()" : lhs.str();
    std::string right =
        convertLeft ? rhs.str() : TypeRelations::operandText(rhs) + "." + method + "()";
    example = ErrorTextMixedNumbers::example(left, op, right);
  } else {
    example = ErrorTextMixedNumbers::method(method);
  }

  return ErrorTextMixedNumbers::mixed(op, l->name(), r->name(), example);
}

std::string BinaryChecker::quotedNames(llvm::ArrayRef<llvm::StringRef> names) {
  std::string list;
  for (size_t i = 0; i < names.size(); ++i) {
    list += (i == 0 ? "" : i + 1 == names.size() ? " or " : ", ") + ("\"" + names[i].str() + "\"");
  }

  return list;
}

BinaryChecker::ResultTypeOfNameKnown BinaryChecker::checkTypeOfName(const Expr *operand,
                                                                    const StringLitExpr *text) {
  std::string name = ToUtf8::toUtf8(text->value);
  if (name == "bigint") {
    std::string what = ErrorTextTypeOfName::bigint();
    std::string bigint = ErrorTextBigInt::none(what);
    this->state.getDiagnostics().error(text->loc(), bigint);

    return err(BinaryCheckerCheckTypeOfNameError{
        .code = BinaryCheckerCheckTypeOfNameErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (!TypeSpelling::isTypeOfName(name)) {
    return BinaryChecker::unknownTypeOfName(operand, name);
  }

  return this->typeOfNameMatches(operand, text, name);
}

BinaryChecker::ResultTypeOfNameKnown BinaryChecker::unknownTypeOfName(const Expr *operand,
                                                                      const std::string &name) {
  if (name == "number") {
    return BinaryChecker::noNumberName(operand);
  }
  std::vector<llvm::StringRef> all(std::begin(kTypeOfNames), std::end(kTypeOfNames));
  std::string list = BinaryChecker::quotedNames(all);
  if (name == "symbol") {
    std::string message = ErrorTextTypeOfName::noSymbol(list);

    return err(BinaryCheckerCheckTypeOfNameError{
        .code = BinaryCheckerCheckTypeOfNameErrorCode::NO_SYMBOL,
        .message = message,
    });
  }
  std::string message = ErrorTextTypeOfName::notName(name, list);

  return err(BinaryCheckerCheckTypeOfNameError{
      .code = BinaryCheckerCheckTypeOfNameErrorCode::NOT_A_NAME,
      .message = message,
  });
}

BinaryChecker::ResultTypeOfNameKnown BinaryChecker::noNumberName(const Expr *operand) {
  std::vector<llvm::StringRef> numbers = BinaryChecker::numberTypeOfNames(operand->type);
  std::string here;
  if (!numbers.empty() && numbers.size() <= 3) {
    std::string quoted = BinaryChecker::quotedNames(numbers);
    here = ErrorTextTypeOfName::forType(operand->type->name(), quoted);
  }
  std::string message = ErrorTextTypeOfName::noNumber(here);

  return err(BinaryCheckerCheckTypeOfNameError{
      .code = BinaryCheckerCheckTypeOfNameErrorCode::NO_NUMBER,
      .message = message,
  });
}

std::vector<llvm::StringRef> BinaryChecker::numberTypeOfNames(const Type *type) {
  std::vector<llvm::StringRef> numbers;
  if (type == nullptr || type->isError()) {
    return numbers;
  }
  for (llvm::StringRef possible : TypeSpelling::typeOfNames(type)) {
    bool numeric = possible.starts_with("int") || possible.starts_with("uint") ||
                   possible == "double" || possible == "decimal";
    if (numeric) {
      numbers.push_back(possible);
    }
  }

  return numbers;
}

BinaryChecker::ResultTypeOfNameKnown
BinaryChecker::typeOfNameMatches(const Expr *operand, const StringLitExpr *text,
                                 const std::string &name) const {
  const Type *type = operand->type;
  bool unchecked = type == nullptr || type->isError() ||
                   this->state.getGenericStore().isCheckingInstance() ||
                   type->nonOptional()->isParam();
  if (unchecked) {
    return ok(text);
  }
  std::vector<llvm::StringRef> names = TypeSpelling::typeOfNames(type);
  bool matches = names.empty() || llvm::is_contained(names, llvm::StringRef(name));
  if (!matches) {
    std::string quoted = BinaryChecker::quotedNames(names);
    std::string message = ErrorTextTypeOfName::never(type->name(), quoted, name);

    return err(BinaryCheckerCheckTypeOfNameError{
        .code = BinaryCheckerCheckTypeOfNameErrorCode::NEVER_MATCHES,
        .message = message,
    });
  }

  return ok(text);
}

BinaryChecker::ResultNoResultOperand BinaryChecker::noResults(const BinaryExpr *bin) {
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
  if (l->nonOptional()->isResult() || r->nonOptional()->isResult()) {
    std::string message = ErrorTextResultEquality::compared(Operators::opSpelling(bin->op));

    return err(BinaryCheckerNoResultsError{
        .code = BinaryCheckerNoResultsErrorCode::RESULTS_COMPARED,
        .message = message,
    });
  }

  return ok(bin);
}

Expr *BinaryChecker::checkEquality(BinaryExpr *bin) {
  Expr *null = this->jsonChecker->checkNullEquality(bin);
  if (null != nullptr) {
    return null;
  }
  this->decimalChecker->checkComparisonOperands(bin);
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
  bin->type = Type::boolTy();
  this->walkTypeOfSide(bin->lhs, bin->rhs);
  this->walkTypeOfSide(bin->rhs, bin->lhs);
  if (l->isError() || r->isError()) {
    return bin;
  }
  if (l->isVoid() || r->isVoid()) {
    std::string message = BinaryChecker::operandTypesText(Operators::opSpelling(bin->op), l, r);
    this->state.getDiagnostics().error(bin->loc(), message);
    return bin;
  }
  if (l->isUndefined() || r->isUndefined()) {
    return bin;
  }
  if (l->nonOptional()->isJson() || r->nonOptional()->isJson()) {
    return this->jsonChecker->checkJsonEquality(bin);
  }
  ResultNoResultOperand plain = BinaryChecker::noResults(bin);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(bin->loc(), plain.error().message);
    return bin;
  }
  if (l->nonOptional()->isParam() || r->nonOptional()->isParam()) {
    return bin;
  }

  return this->checkEqualOperands(bin);
}

void BinaryChecker::walkTypeOfSide(const Expr *side, const Expr *other) {
  auto *typeOf = dyn_cast<UnaryExpr>(side);
  auto *text = dyn_cast<StringLitExpr>(other);
  if (typeOf == nullptr || typeOf->op != UnaryOp::TypeOf || text == nullptr) {
    return;
  }
  ResultTypeOfNameKnown known = this->checkTypeOfName(typeOf->operand, text);
  if (known.isErr() &&
      known.error().code != BinaryCheckerCheckTypeOfNameErrorCode::ALREADY_REPORTED) {
    this->state.getDiagnostics().error(text->loc(), known.error().message);
  }
}

Expr *BinaryChecker::checkEqualOperands(BinaryExpr *bin) {
  this->adaptLiterals(bin);
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
  if (l->isInteger() && r->isInteger()) {
    return bin;
  }
  const Type *common = TypeRelations::comparisonType(l, r);
  if (common == nullptr) {
    this->reportIncomparable(bin);
    return bin;
  }
  if (common->nonOptional()->isFunction()) {
    this->state.getInventoryStore().markComparesFunctions();
  }
  bin->lhs = this->typeRelations->convert(bin->lhs, common);
  bin->rhs = this->typeRelations->convert(bin->rhs, common);

  return bin;
}

void BinaryChecker::reportIncomparable(const BinaryExpr *bin) {
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
  if (l->nonOptional()->isDecimal() || r->nonOptional()->isDecimal()) {
    return this->decimalChecker->reportDecimalMix(bin);
  }
  bool numbers = l->nonOptional()->isNumeric() && r->nonOptional()->isNumeric() &&
                 !l->isOptional() && !r->isOptional();
  std::string message = numbers
                            ? BinaryChecker::mixedNumbersText(bin)
                            : BinaryChecker::operandTypesText(Operators::opSpelling(bin->op), l, r);
  this->state.getDiagnostics().error(bin->loc(), message);
}

BinaryChecker::ResultNullishResolved BinaryChecker::checkNullish(BinaryExpr *bin,
                                                                 const Type *expected) {
  bin->lhs = this->exprChecker->checkExpr(bin->lhs, expected);
  if (bin->lhs->type->nonOptional()->isMatch() && isa<ArrayLitExpr>(bin->rhs)) {
    bin->lhs = this->matchChecker->matchItems(bin->lhs);
  }
  const Type *l = bin->lhs->type;
  Flow afterLhs = this->state.getFlowState().getFlow();
  const Type *hint = l->isUndefined() || l->isError() ? expected : l->nonOptional();
  bin->rhs = this->exprChecker->checkExpr(bin->rhs, hint);
  this->state.getFlowState().replaceFlow(
      FlowChecker::join(afterLhs, this->state.getFlowState().getFlow()));
  const Type *r = bin->rhs->type;
  bin->type = Type::error();
  if (l->isError() || r->isError()) {
    if (this->state.getFunctionStack().isInsideTrial() && l->isError() != r->isError()) {
      bin->type = l->isError() ? r : l->nonOptional();
    }
    return ok(bin);
  }
  if (l->isVoid() || r->isVoid()) {
    std::string message = BinaryChecker::operandTypesText("??", l, r);

    return err(BinaryCheckerCheckNullishError{
        .code = BinaryCheckerCheckNullishErrorCode::VOID_OPERAND,
        .message = message,
    });
  }
  if (l->isUndefined()) {
    bin->type = r;
    return ok(bin);
  }
  const Type *base = r->isUndefined()
                         ? l->nonOptional()
                         : LiteralTypes::unionUnify(l->nonOptional(), r->nonOptional());
  if (!base || base->isError()) {
    std::string message = BinaryChecker::operandTypesText("??", l, r);

    return err(BinaryCheckerCheckNullishError{
        .code = BinaryCheckerCheckNullishErrorCode::NO_COMMON_TYPE,
        .message = message,
    });
  }
  const Type *result = r->isOptional() || r->isUndefined() ? Type::optional(base) : base;
  bin->lhs =
      this->typeRelations->convert(bin->lhs, l->isOptional() ? Type::optional(base) : result);
  bin->rhs = this->typeRelations->convert(bin->rhs, result);
  bin->type = result;

  return ok(bin);
}

Expr *BinaryChecker::toInt32(Expr *expr) {
  return expr->type->isInt() || expr->type->isError()
             ? expr
             : this->typeRelations->convert(expr, Type::intTy());
}

}
