#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/program/library/decimal/decimal_static_table/decimal_static_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/binary_checker/binary_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/semantic/declare/prelude/prelude.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "runtime/library/decimal/decimal_text/decimal_text.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cctype>
#include <cstring>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr int64_t kCountLimit = 1000000000;
constexpr int64_t kLowestBase = 2;
constexpr int64_t kHighestBase = 36;
constexpr int64_t kSafeIntegerLimit = 9007199254740991;


}

DecimalChecker::DecimalChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->prelude = &all.prelude;
  this->exprChecker = &all.exprChecker;
  this->indexProofs = &all.indexProofs;
  this->literalTypes = &all.literalTypes;
  this->typeRelations = &all.typeRelations;
  this->binaryChecker = &all.binaryChecker;
  this->constantFolder = &all.constantFolder;
  this->argumentChecker = &all.argumentChecker;
}

DecimalChecker::ResultRoundingMode
DecimalChecker::checkRoundingModeText(const std::u16string &text) const {
  int mode = DecimalLiteral::roundingMode(text);
  if (mode < 0) {
    auto written = ToUtf8::toUtf8(text);
    auto message = ErrorTextRoundingMode::unknown(written);

    return err(DecimalCheckerRoundingModeError{
        .code = DecimalCheckerRoundingModeErrorCode::UNKNOWN_MODE,
        .message = message,
    });
  }

  return ok(mode);
}

DecimalChecker::ResultLiteralRange DecimalChecker::checkLiteralRange(const Expr *literal) const {
  auto value = DecimalLiteral::fromExpr(literal);
  if (!value.has_value()) {
    auto message = ErrorTextDecimalLit::unreadable();

    return err(DecimalCheckerLiteralRangeError{
        .code = DecimalCheckerLiteralRangeErrorCode::UNREADABLE,
        .message = message,
    });
  }
  if (!value->isZero() && value->top() > kDecimalMaxExp) {
    auto message = ErrorTextDecimalLit::outOfRange(kDecimalMaxExp + 1);

    return err(DecimalCheckerLiteralRangeError{
        .code = DecimalCheckerLiteralRangeErrorCode::OUT_OF_RANGE,
        .message = message,
    });
  }

  return ok(value.value());
}

std::pair<llvm::StringRef, llvm::StringRef>
DecimalChecker::sidesOf(const BinaryExpr *binary) const {
  llvm::StringRef source = binary->source;
  const char *at = binary->loc().getPointer();
  size_t length = std::strlen(Operators::opSpelling(binary->op));
  if (source.empty() || at < source.begin() || at + length > source.end()) {
    return {};
  }

  return {llvm::StringRef(source.begin(), static_cast<size_t>(at - source.begin())).trim(),
          llvm::StringRef(at + length, static_cast<size_t>(source.end() - at - length)).trim()};
}

bool DecimalChecker::isLiteralTree(const Expr *expr) const {
  for (const Expr *node : ExprParts::operatorTree(expr)) {
    if (!this->isLiteralNode(node)) {
      return false;
    }
  }

  return true;
}

bool DecimalChecker::isLiteralNode(const Expr *expr) const {
  if (isa<IntLitExpr>(expr) || isa<FloatLitExpr>(expr)) {
    return true;
  }
  const auto *unary = dyn_cast<UnaryExpr>(expr);
  if (unary != nullptr) {
    return unary->op == UnaryOp::Neg || unary->op == UnaryOp::Plus;
  }
  const auto *binary = dyn_cast<BinaryExpr>(expr);
  if (binary == nullptr) {
    return false;
  }

  return this->isArithmeticOperator(binary->op);
}

bool DecimalChecker::isArithmeticOperator(BinaryOp op) const {
  return op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul || op == BinaryOp::Div ||
         op == BinaryOp::Rem || op == BinaryOp::Pow;
}

bool DecimalChecker::isFittingLiteral(const Expr *expr, const Type *type) const {
  const auto *unary = dyn_cast<UnaryExpr>(expr);
  bool isNegated = unary != nullptr && unary->op == UnaryOp::Neg;
  const Expr *operand = isNegated ? unary->operand : expr;
  const auto *literal = dyn_cast<IntLitExpr>(operand);
  if (literal == nullptr) {
    return false;
  }
  if (isNegated) {
    return literal->value == 0 ||
           (!type->isUnsigned() && literal->value <= uint64_t(0) - uint64_t(type->intMin()));
  }

  return literal->value <= type->intMax();
}

const Type *DecimalChecker::literalHint(const Expr *other, const Type *hint) const {
  return other->type != nullptr && other->type->nonOptional()->isDecimal() ? Type::decimalTy()
                                                                           : hint;
}

void DecimalChecker::walkComparisonOperands(BinaryExpr *binary) {
  if (this->isComparedLiteral(binary->lhs) && !this->isComparedLiteral(binary->rhs)) {
    binary->rhs = this->exprChecker->checkExpr(binary->rhs);
    const auto *leftHint = this->comparisonHint(binary->rhs, binary->lhs);
    binary->lhs = this->exprChecker->checkExpr(binary->lhs, leftHint);
    return;
  }
  binary->lhs = this->exprChecker->checkExpr(binary->lhs);
  bool isLiteral = this->isComparedLiteral(binary->rhs);
  const auto *rightHint = isLiteral ? this->comparisonHint(binary->lhs, binary->rhs) : nullptr;
  binary->rhs = this->exprChecker->checkExpr(binary->rhs, rightHint);
}

bool DecimalChecker::isComparedLiteral(const Expr *expr) const {
  const auto *unary = dyn_cast<UnaryExpr>(expr);
  bool isNegated = unary != nullptr && unary->op == UnaryOp::Neg;
  const Expr *operand = isNegated ? unary->operand : expr;
  const auto *member = dyn_cast<MemberExpr>(operand);
  const auto *owner = member != nullptr ? dyn_cast<NameExpr>(member->object) : nullptr;

  return this->isLiteralTree(operand) || isa<StringLitExpr>(operand) || isa<BoolLitExpr>(operand) ||
         (owner != nullptr && this->scopes->lookup(owner->name).enumDecl() != nullptr);
}

const Type *DecimalChecker::comparisonHint(const Expr *other, const Expr *self) const {
  const Type *type = other->type != nullptr ? other->type->nonOptional() : nullptr;
  if (type != nullptr && (type->isLiteral() || type->isUnion())) {
    return other->type;
  }
  if (type != nullptr && type->isNumeric() &&
      (type->isFloat() || this->isFittingLiteral(self, type))) {
    return type;
  }

  return this->literalHint(other, nullptr);
}

DecimalChecker::ResultDecimalLiteralChecked
DecimalChecker::checkDecimalLiteralRange(const Expr *literal) {
  auto range = this->checkLiteralRange(literal);
  if (range.isErr()) {
    return err(range.error());
  }

  return ok();
}

void DecimalChecker::reportDecimalMix(const BinaryExpr *binary) {
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  const char *spelling = Operators::opSpelling(binary->op);
  bool isLeftDecimal = leftType->nonOptional()->isDecimal();
  const Type *other = isLeftDecimal ? rightType : leftType;
  if (other->isInteger()) {
    auto [lhs, rhs] = this->sidesOf(binary);
    auto message = ErrorTextDecimalMix::integer(spelling, leftType->name(), rightType->name(), lhs,
                                                rhs, isLeftDecimal);
    this->state.getDiagnostics().error(binary->loc(), message);
    return;
  }
  if (!other->nonOptional()->isFloat()) {
    auto message = this->binaryChecker->operandTypesText(spelling, leftType, rightType);
    this->state.getDiagnostics().error(binary->loc(), message);
    return;
  }
  auto [lhs, rhs] = this->sidesOf(binary);
  auto message = ErrorTextDecimalMix::inexact(spelling, leftType->name(), rightType->name(), lhs,
                                              rhs, isLeftDecimal, other->isFloat());
  this->state.getDiagnostics().error(binary->loc(), message);
}

Expr *DecimalChecker::checkDecimalArithmetic(BinaryExpr *binary) {
  binary->type = Type::error();
  if (binary->op == BinaryOp::Pow) {
    return this->checkDecimalPower(binary);
  }
  if (!this->convertDecimalSides(binary)) {
    return binary;
  }
  this->walkDecimalDivisor(binary->op, binary->rhs);
  binary->type = Type::decimalTy();

  return binary;
}

Expr *DecimalChecker::checkDecimalPower(BinaryExpr *binary) {
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  if (!leftType->isDecimal()) {
    const char *spelling = Operators::opSpelling(BinaryOp::Pow);
    auto message = this->binaryChecker->operandTypesText(spelling, leftType, rightType);
    this->state.getDiagnostics().error(binary->loc(), message);
    return binary;
  }
  if (!rightType->isInteger()) {
    auto message = ErrorTextDecimalPower::exponent(rightType);
    this->state.getDiagnostics().error(binary->rhs->loc(), message);
    return binary;
  }
  binary->rhs = this->typeRelations->convert(binary->rhs, Type::intTy());
  binary->type = Type::decimalTy();

  return binary;
}

bool DecimalChecker::convertDecimalSides(BinaryExpr *binary) {
  for (Expr **side : {&binary->lhs, &binary->rhs}) {
    if (!this->convertDecimalSide(*side)) {
      this->reportDecimalMix(binary);
      return false;
    }
  }

  return true;
}

bool DecimalChecker::convertDecimalSide(Expr *&side) {
  const Type *type = side->type;
  if (type->isDecimal()) {
    return true;
  }
  if (!type->isInteger() || !this->typeRelations->isIntLiteralTree(side)) {
    return false;
  }
  side = this->typeRelations->convert(side, Type::decimalTy());

  return true;
}

Expr *DecimalChecker::checkDecimalComparison(BinaryExpr *binary) {
  binary->type = Type::boolTy();
  this->convertDecimalSides(binary);

  return binary;
}

void DecimalChecker::walkDecimalAssign(AssignExpr *assign, BinaryOp op) {
  if (op == BinaryOp::Pow) {
    return this->walkDecimalPowerAssign(assign);
  }
  bool isArithmetic = op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul ||
                      op == BinaryOp::Div || op == BinaryOp::Rem;
  if (!isArithmetic) {
    std::string spelling = std::string(Operators::opSpelling(op)) + "=";
    const Type *valueType = assign->value->type;
    auto message = this->binaryChecker->operandTypesText(spelling, assign->type, valueType);
    this->state.getDiagnostics().error(assign->loc(), message);
    return;
  }
  assign->value = this->typeRelations->coerce(assign->value, Type::decimalTy());
  this->walkDecimalDivisor(op, assign->value);
}

void DecimalChecker::walkDecimalPowerAssign(AssignExpr *assign) {
  const Type *valueType = assign->value->type;
  if (!valueType->isInteger()) {
    auto message = ErrorTextDecimalPower::exponent(valueType);
    this->state.getDiagnostics().error(assign->value->loc(), message);
    return;
  }
  assign->value = this->typeRelations->convert(assign->value, Type::intTy());
}

void DecimalChecker::walkDecimalDivisor(BinaryOp op, const Expr *divisor) {
  if (op != BinaryOp::Div && op != BinaryOp::Rem) {
    return;
  }
  const auto *value = ExprParts::withoutImplicitCasts(divisor);
  auto literal = DecimalLiteral::fromExpr(value);
  bool isZero =
      literal.has_value() ? literal->isZero() : this->constantFolder->intConstant(value) == 0;
  if (isZero) {
    auto message = ErrorTextDecimalPower::divZero();
    this->state.getDiagnostics().error(divisor->loc(), message);
  }
}

Expr *DecimalChecker::checkDecimalCall(CallExpr *call, llvm::SMLoc loc) {
  call->type = Type::error();
  auto arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  Expr *&arg = call->args[0];
  bool isLiteral = DecimalLiteral::fromExpr(arg).has_value();
  const Type *hint = isLiteral ? Type::decimalTy() : nullptr;
  arg = this->exprChecker->checkExpr(arg, hint);
  const Type *type = arg->type;
  if (type->isError()) {
    return call;
  }
  if (this->isDecimalText(type)) {
    return this->checkDecimalText(call);
  }
  bool isNumber = type->isInteger() || type->isDecimal() || type->isFloat();
  if (!isNumber) {
    auto message = ErrorTextDecimalOf::takes(type->name(), type->isOptional());
    this->state.getDiagnostics().error(arg->loc(), message);
    return call;
  }
  if (type->isInteger() || type->isDecimal()) {
    arg = this->typeRelations->convert(arg, Type::decimalTy());
  }
  call->target = BuiltinCall{.builtin = Builtin::DecimalOf};
  call->type = Type::decimalTy();

  return call;
}

bool DecimalChecker::isDecimalText(const Type *type) const {
  if (type->isOptional()) {
    return false;
  }
  const Type *text = type->nonOptional();
  auto isStringLiteral = std::bind_front(&DecimalChecker::isStringLiteralType, this);

  return text->isString() || text->isStringLiteral() ||
         (text->isUnion() && llvm::all_of(text->members(), isStringLiteral));
}

bool DecimalChecker::isStringLiteralType(const Type *type) const {
  return type->isStringLiteral();
}

Expr *DecimalChecker::checkDecimalText(CallExpr *call) {
  Expr *&arg = call->args[0];
  auto texts = this->literalTypes->knownTexts(arg);
  if (texts.has_value() && this->reportBadDecimalText(texts.value(), arg)) {
    return call;
  }
  if (!arg->type->nonOptional()->isString()) {
    arg = this->typeRelations->convert(arg, Type::string());
  }
  call->target = BuiltinCall{.builtin = Builtin::DecimalOf};
  if (texts.has_value()) {
    call->type = Type::decimalTy();
    return call;
  }
  const auto *decimalError = this->prelude->builtinType(PreludeType::DECIMAL_ERROR);
  call->type = Type::result(Type::decimalTy(), decimalError);

  return call;
}

bool DecimalChecker::reportBadDecimalText(llvm::ArrayRef<std::string> texts, const Expr *arg) {
  for (const std::string &text : texts) {
    if (!this->isDecimalNumberText(text)) {
      auto message = ErrorTextDecimalOf::notNumber(text);
      this->state.getDiagnostics().error(arg->loc(), message);
      return true;
    }
  }

  return false;
}

bool DecimalChecker::isDecimalNumberText(const std::string &text) const {
  auto units = llvm::SmallVector<llvm::UTF16, 32>();
  llvm::convertUTF8ToUTF16String(text, units);
  const auto *data = reinterpret_cast<const uint16_t *>(units.data());

  return tz_decimal_text_ok(data, units.size());
}

Expr *DecimalChecker::checkDecimalStatic(CallExpr *call, MemberExpr *member) {
  llvm::StringRef name = member->name;
  call->type = Type::error();
  const auto *function = DecimalStaticTable::find(name);
  if (function == nullptr) {
    auto message = ErrorTextDecimalStatic::noMember(name);
    this->state.getDiagnostics().error(member->loc(), message);
    this->argumentChecker->walkArgsLoosely(call);
    return call;
  }
  Builtin builtin = function->builtin;
  const auto *decimal = Type::decimalTy();
  bool isSomeValue = false;
  bool isOk = true;
  for (size_t index = 0; index < call->args.size(); ++index) {
    if (call->spreads(index)) {
      isOk &= this->takeDecimalSpread(call->args[index]);
      isSomeValue |= this->indexProofs->canProveNotEmpty(call->args[index]);
    } else {
      call->args[index] = this->argumentChecker->checkArg(call->args[index], decimal);
      isSomeValue = true;
    }
  }
  if (!isOk) {
    return call;
  }
  if (call->args.empty() && builtin != Builtin::DecSum) {
    auto message = ErrorTextDecimalStatic::noValue(name);
    this->state.getDiagnostics().error(member->loc(), message);
    return call;
  }
  call->target = BuiltinCall{.builtin = builtin};
  call->type = isSomeValue || builtin == Builtin::DecSum ? decimal : Type::optional(decimal);

  return call;
}

bool DecimalChecker::takeDecimalSpread(Expr *&arg) {
  const auto *array = Type::array(Type::decimalTy());
  arg = this->exprChecker->checkExpr(arg, array);
  bool isFitting = arg->type == array || arg->type->isError();
  if (!isFitting) {
    auto message = ErrorTextDecimalStatic::spread(arg->type->name());
    this->state.getDiagnostics().error(arg->loc(), message);
  }

  return isFitting;
}

Expr *DecimalChecker::checkRoundingMode(Expr *arg) {
  auto *checked = this->argumentChecker->checkArg(arg, Type::string());
  if (checked->type->isError()) {
    return checked;
  }
  auto *literal = dyn_cast<StringLitExpr>(checked);
  if (literal != nullptr) {
    auto mode = this->checkRoundingModeText(literal->value);
    if (mode.isErr()) {
      this->state.getDiagnostics().error(checked->loc(), mode.error().message);
    }
    return checked;
  }
  auto modes = this->literalTypes->knownTexts(checked);
  if (!modes.has_value()) {
    auto message = ErrorTextRoundingMode::notKnown();
    this->state.getDiagnostics().error(checked->loc(), message);
    return checked;
  }
  this->walkKnownModes(modes.value(), checked);

  return checked;
}

void DecimalChecker::walkKnownModes(llvm::ArrayRef<std::string> modes, const Expr *arg) {
  for (const std::string &mode : modes) {
    std::u16string text(mode.begin(), mode.end());
    auto known = this->checkRoundingModeText(text);
    if (known.isErr()) {
      this->state.getDiagnostics().error(arg->loc(), known.error().message);
      return;
    }
  }
}

Expr *DecimalChecker::checkDecimalCount(Expr *arg, llvm::StringRef what, int64_t low,
                                        int64_t high) {
  arg = this->exprChecker->checkExpr(arg, Type::intTy());
  const Type *type = arg->type;
  if (type->isError()) {
    return arg;
  }
  if (!type->isInteger()) {
    auto message = ErrorTextDecimalCount::notInt(what, type->name(), type->isOptional());
    this->state.getDiagnostics().error(arg->loc(), message);
    return arg;
  }
  auto between = this->indexProofs->requireBetween(arg, low, high, what);
  if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
    this->state.getDiagnostics().error(arg->loc(), between.error().message);
  }

  return this->typeRelations->convert(arg, Type::intTy());
}

Expr *DecimalChecker::checkDecimalMethod(CallExpr *call, MemberExpr *member) {
  const auto *method = DecimalMethodTable::find(member->name);
  if (method == nullptr) {
    return this->reportDecimalMember(call, member);
  }
  if (method->builtin == Builtin::DecDecimalPlaces && !call->args.empty()) {
    auto message = ErrorTextDecimalMethod::places();
    this->state.getDiagnostics().error(member->loc(), message);
    this->argumentChecker->walkArgsLoosely(call);
    return call;
  }
  auto arity = this->argumentChecker->checkArity(call, method->minArgs, method->maxArgs);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  for (size_t index = 0; index < call->args.size(); ++index) {
    call->args[index] =
        this->checkDecimalArg(call->args[index], method->args[index], method->builtin);
  }
  if (method->builtin == Builtin::DecDiv) {
    this->walkDecimalDivisor(BinaryOp::Div, call->args[0]);
  }
  call->target = BuiltinCall{.builtin = method->builtin};
  call->type = this->decimalResultType(method->result);

  return call;
}

Expr *DecimalChecker::reportDecimalMember(CallExpr *call, MemberExpr *member) {
  auto use = ErrorTextDecimalMethod::replacementFor(member->name);
  auto message = ErrorTextDecimalMethod::noMember(member->name, use);
  this->state.getDiagnostics().error(member->loc(), message);
  this->argumentChecker->walkArgsLoosely(call);

  return call;
}

Expr *DecimalChecker::checkDecimalArg(Expr *arg, DecimalArg kind, Builtin builtin) {
  switch (kind) {
    case DecimalArg::DECIMAL:
      return this->argumentChecker->checkArg(arg, Type::decimalTy());
    case DecimalArg::PLACES: {
      bool isPositive = builtin == Builtin::DecDiv || builtin == Builtin::DecToExponential;
      auto places = ErrorTextDecimalCount::places();
      int64_t low = isPositive ? 0 : -kCountLimit;
      return this->checkDecimalCount(arg, places, low, kCountLimit);
    }
    case DecimalArg::DIGITS: {
      auto digits = ErrorTextDecimalCount::digits();
      return this->checkDecimalCount(arg, digits, 1, kCountLimit);
    }
    case DecimalArg::BASE: {
      auto base = ErrorTextDecimalCount::base();
      return this->checkDecimalCount(arg, base, kLowestBase, kHighestBase);
    }
    case DecimalArg::POWER: {
      auto power = ErrorTextDecimalCount::power();
      return this->checkDecimalCount(arg, power, -kSafeIntegerLimit, kSafeIntegerLimit);
    }
    case DecimalArg::NONE:
    case DecimalArg::ROUNDING:
      return this->checkRoundingMode(arg);
  }

  return this->checkRoundingMode(arg);
}

const Type *DecimalChecker::decimalResultType(DecimalResult result) const {
  switch (result) {
    case DecimalResult::DECIMAL:
      return Type::decimalTy();
    case DecimalResult::STRING:
      return Type::string();
    case DecimalResult::BOOL:
      return Type::boolTy();
    case DecimalResult::INT:
      return Type::intTy();
    case DecimalResult::DECIMAL_LIST:
      return Type::array(Type::decimalTy());
  }

  return Type::decimalTy();
}

}
