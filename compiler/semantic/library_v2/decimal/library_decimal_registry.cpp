#include "compiler/semantic/library_v2/decimal/library_decimal_registry.h"
#include "compiler/program/library/decimal/decimal_static_table/decimal_static_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/code/operators/code_operators_registry_errors.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "runtime/library/decimal/decimal_text/decimal_text.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cstring>
#include <functional>

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr int64_t kCountLimit = 1000000000;
constexpr int64_t kLowestBase = 2;
constexpr int64_t kHighestBase = 36;
constexpr int64_t kSafeIntegerLimit = 9007199254740991;

}

LibraryDecimalRegistry::LibraryDecimalRegistry(SemanticState &owner)
    : state(owner) {}

void LibraryDecimalRegistry::checkDecimalLiteralRange(const CheckDecimalLiteralRange &task) {
  auto range = this->checkLiteralRange(task.literal);
  if (range.isErr()) {
    return this->state.diagnostics.error(task.literal->loc(), range.error().message);
  }
}

void LibraryDecimalRegistry::walkComparisonOperands(const WalkComparisonOperands &task) {
  BinaryExpr *binary = task.binary;
  auto &tasks = this->state.walk.tasks;
  bool isLiteralFirst =
      this->isComparedLiteral(binary->lhs) && !this->isComparedLiteral(binary->rhs);
  if (isLiteralFirst) {
    tasks.pushTask(AfterComparedRight{.binary = binary});

    return tasks.pushTask(CheckExprSlot{.slot = &binary->rhs, .expected = nullptr});
  }
  tasks.pushTask(AfterComparedLeft{.binary = binary});

  return tasks.pushTask(CheckExprSlot{.slot = &binary->lhs, .expected = nullptr});
}

void LibraryDecimalRegistry::afterComparedRight(const AfterComparedRight &task) {
  BinaryExpr *binary = task.binary;
  const auto *leftHint = this->comparisonHint(binary->rhs, binary->lhs);

  return this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &binary->lhs, .expected = leftHint});
}

void LibraryDecimalRegistry::afterComparedLeft(const AfterComparedLeft &task) {
  BinaryExpr *binary = task.binary;
  bool isLiteral = this->isComparedLiteral(binary->rhs);
  const auto *rightHint = isLiteral ? this->comparisonHint(binary->lhs, binary->rhs) : nullptr;

  return this->state.walk.tasks.pushTask(
      CheckExprSlot{.slot = &binary->rhs, .expected = rightHint});
}

void LibraryDecimalRegistry::reportDecimalMix(const ReportDecimalMix &task) {
  return this->reportMix(task.binary);
}

void LibraryDecimalRegistry::reportMix(const BinaryExpr *binary) {
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  const char *spelling = Operators::opSpelling(binary->op);
  bool isLeftDecimal = leftType->nonOptional()->isDecimal();
  const Type *other = isLeftDecimal ? rightType : leftType;
  if (other->isInteger()) {
    auto [lhs, rhs] = this->sidesOf(binary);
    auto message = ErrorTextDecimalMix::integer(spelling, leftType->name(), rightType->name(), lhs,
                                                rhs, isLeftDecimal);

    return this->state.diagnostics.error(binary->loc(), message);
  }
  if (!other->nonOptional()->isFloat()) {
    auto message = this->operandTypesText(spelling, leftType, rightType);

    return this->state.diagnostics.error(binary->loc(), message);
  }
  auto [lhs, rhs] = this->sidesOf(binary);
  auto message = ErrorTextDecimalMix::inexact(spelling, leftType->name(), rightType->name(), lhs,
                                              rhs, isLeftDecimal, other->isFloat());
  this->state.diagnostics.error(binary->loc(), message);
}

void LibraryDecimalRegistry::checkDecimalArithmetic(const CheckDecimalArithmetic &task) {
  BinaryExpr *binary = task.binary;
  auto &tasks = this->state.walk.tasks;
  binary->type = Type::error();
  tasks.pushChecked(binary);
  if (binary->op == BinaryOp::Pow) {
    return this->checkDecimalPower(binary);
  }

  return tasks.pushTask(
      ConvertDecimalSide{.binary = binary, .isRight = false, .isArithmetic = true});
}

void LibraryDecimalRegistry::checkDecimalComparison(const CheckDecimalComparison &task) {
  BinaryExpr *binary = task.binary;
  auto &tasks = this->state.walk.tasks;
  binary->type = Type::boolTy();
  tasks.pushChecked(binary);

  return tasks.pushTask(
      ConvertDecimalSide{.binary = binary, .isRight = false, .isArithmetic = false});
}

void LibraryDecimalRegistry::convertDecimalSide(const ConvertDecimalSide &task) {
  BinaryExpr *binary = task.binary;
  auto &tasks = this->state.walk.tasks;
  Expr **side = task.isRight ? &binary->rhs : &binary->lhs;
  const Type *type = (*side)->type;
  bool isWholeLiteral = type->isInteger() && this->state.code.conversions.isIntLiteralTree(*side);
  if (!type->isDecimal() && !isWholeLiteral) {
    return this->reportMix(binary);
  }
  if (task.isRight && task.isArithmetic) {
    tasks.pushTask(FinishDecimalArithmetic{.binary = binary});
  }
  if (!task.isRight) {
    tasks.pushTask(
        ConvertDecimalSide{.binary = binary, .isRight = true, .isArithmetic = task.isArithmetic});
  }
  if (type->isDecimal()) {
    return;
  }

  return tasks.pushTask(ConvertSlot{.slot = side, .to = Type::decimalTy()});
}

void LibraryDecimalRegistry::finishDecimalArithmetic(const FinishDecimalArithmetic &task) {
  BinaryExpr *binary = task.binary;
  this->walkDecimalDivisor(binary->op, binary->rhs);
  binary->type = Type::decimalTy();
}

void LibraryDecimalRegistry::checkDecimalPower(BinaryExpr *binary) {
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  if (!leftType->isDecimal()) {
    const char *spelling = Operators::opSpelling(BinaryOp::Pow);
    auto message = this->operandTypesText(spelling, leftType, rightType);

    return this->state.diagnostics.error(binary->loc(), message);
  }
  if (!rightType->isInteger()) {
    auto message = ErrorTextDecimalPower::exponent(rightType);

    return this->state.diagnostics.error(binary->rhs->loc(), message);
  }
  binary->type = Type::decimalTy();

  return this->state.walk.tasks.pushTask(ConvertSlot{.slot = &binary->rhs, .to = Type::intTy()});
}

void LibraryDecimalRegistry::walkDecimalAssign(const WalkDecimalAssign &task) {
  AssignExpr *assign = task.assign;
  BinaryOp op = task.op;
  auto &tasks = this->state.walk.tasks;
  const Type *valueType = assign->value->type;
  if (op == BinaryOp::Pow && !valueType->isInteger()) {
    auto message = ErrorTextDecimalPower::exponent(valueType);

    return this->state.diagnostics.error(assign->value->loc(), message);
  }
  if (op == BinaryOp::Pow) {
    return tasks.pushTask(ConvertSlot{.slot = &assign->value, .to = Type::intTy()});
  }
  bool isArithmetic = op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul ||
                      op == BinaryOp::Div || op == BinaryOp::Rem;
  if (!isArithmetic) {
    std::string spelling = std::string(Operators::opSpelling(op)) + "=";
    auto message = this->operandTypesText(spelling, assign->type, valueType);

    return this->state.diagnostics.error(assign->loc(), message);
  }
  tasks.pushTask(AfterDecimalAssign{.assign = assign, .op = op});

  return tasks.pushTask(CoerceSlot{.slot = &assign->value, .to = Type::decimalTy(), .what = ""});
}

void LibraryDecimalRegistry::afterDecimalAssign(const AfterDecimalAssign &task) {
  return this->walkDecimalDivisor(task.op, task.assign->value);
}

void LibraryDecimalRegistry::walkDecimalDivisor(BinaryOp op, const Expr *divisor) {
  if (op != BinaryOp::Div && op != BinaryOp::Rem) {
    return;
  }
  const auto *value = ExprParts::withoutImplicitCasts(divisor);
  auto literal = DecimalLiteral::fromExpr(value);
  bool isZero =
      literal.has_value() ? literal->isZero() : this->state.code.constants.intConstant(value) == 0;
  if (isZero) {
    auto message = ErrorTextDecimalPower::divZero();
    this->state.diagnostics.error(divisor->loc(), message);
  }
}

void LibraryDecimalRegistry::checkDecimalCall(const CheckDecimalCall &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  tasks.pushChecked(call);
  auto arity = this->state.library.common.checkArity(call, 1, 1);
  if (arity.isErr()) {
    return this->state.diagnostics.error(task.loc, arity.error().message);
  }
  bool isLiteral = DecimalLiteral::fromExpr(call->args[0]).has_value();
  const Type *hint = isLiteral ? Type::decimalTy() : nullptr;
  tasks.pushTask(AfterDecimalCallArg{.call = call});

  return tasks.pushTask(CheckExprSlot{.slot = &call->args[0], .expected = hint});
}

void LibraryDecimalRegistry::afterDecimalCallArg(const AfterDecimalCallArg &task) {
  CallExpr *call = task.call;
  auto &tasks = this->state.walk.tasks;
  const Expr *arg = call->args[0];
  const Type *type = arg->type;
  if (type->isError()) {
    return;
  }
  if (this->isDecimalText(type)) {
    return this->checkDecimalText(call);
  }
  bool isNumber = type->isInteger() || type->isDecimal() || type->isFloat();
  if (!isNumber) {
    auto message = ErrorTextDecimalOf::takes(type->name(), type->isOptional());

    return this->state.diagnostics.error(arg->loc(), message);
  }
  call->target = BuiltinCall{.builtin = Builtin::DecimalOf};
  call->type = Type::decimalTy();
  if (type->isFloat()) {
    return;
  }

  return tasks.pushTask(ConvertSlot{.slot = &call->args[0], .to = Type::decimalTy()});
}

void LibraryDecimalRegistry::checkDecimalText(CallExpr *call) {
  auto &tasks = this->state.walk.tasks;
  const Expr *arg = call->args[0];
  auto texts = this->state.library.common.knownTexts(arg);
  if (texts.has_value() && this->reportBadDecimalText(texts.value(), arg)) {
    return;
  }
  call->target = BuiltinCall{.builtin = Builtin::DecimalOf};
  call->type = Type::decimalTy();
  if (!texts.has_value()) {
    tasks.pushTask(FinishDecimalText{.call = call});
    tasks.pushTask(PreludeTypeOf{.type = PreludeType::DECIMAL_ERROR});
  }
  if (arg->type->nonOptional()->isString()) {
    return;
  }

  return tasks.pushTask(ConvertSlot{.slot = &call->args[0], .to = Type::string()});
}

void LibraryDecimalRegistry::finishDecimalText(const FinishDecimalText &task) {
  const Type *decimalError = this->state.walk.tasks.takeAnswer();
  task.call->type = Type::result(Type::decimalTy(), decimalError);
}

void LibraryDecimalRegistry::checkDecimalStatic(const CheckDecimalStatic &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  call->type = Type::error();
  tasks.pushChecked(call);
  const auto *function = DecimalStaticTable::find(member->name);
  if (function == nullptr) {
    auto message = ErrorTextDecimalStatic::noMember(member->name);
    this->state.diagnostics.error(member->loc(), message);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  tasks.pushTask(FinishDecimalStatic{.call = call, .member = member, .builtin = function->builtin});
  for (size_t index = call->args.size(); index > 0; --index) {
    Expr **slot = &call->args[index - 1];
    if (call->spreads(index - 1)) {
      tasks.pushTask(AfterDecimalSpread{.slot = slot});
      tasks.pushTask(CheckExprSlot{.slot = slot, .expected = Type::array(Type::decimalTy())});
    } else {
      tasks.pushTask(CheckArgSlot{.slot = slot, .to = Type::decimalTy()});
    }
  }
}

void LibraryDecimalRegistry::afterDecimalSpread(const AfterDecimalSpread &task) {
  const Expr *arg = *task.slot;
  if (this->isFittingSpread(arg)) {
    return;
  }
  auto message = ErrorTextDecimalStatic::spread(arg->type->name());
  this->state.diagnostics.error(arg->loc(), message);
}

void LibraryDecimalRegistry::finishDecimalStatic(const FinishDecimalStatic &task) {
  CallExpr *call = task.call;
  Builtin builtin = task.builtin;
  bool isOk = true;
  bool isSomeValue = false;
  for (size_t index = 0; index < call->args.size(); ++index) {
    bool isSpread = call->spreads(index);
    const Expr *arg = call->args[index];
    isOk = isOk && (!isSpread || this->isFittingSpread(arg));
    isSomeValue = isSomeValue || !isSpread || this->state.flow.proofs.canProveNotEmpty(arg);
  }
  if (!isOk) {
    return;
  }
  if (call->args.empty() && builtin != Builtin::DecSum) {
    auto message = ErrorTextDecimalStatic::noValue(task.member->name);

    return this->state.diagnostics.error(task.member->loc(), message);
  }
  const auto *decimal = Type::decimalTy();
  call->target = BuiltinCall{.builtin = builtin};
  call->type = isSomeValue || builtin == Builtin::DecSum ? decimal : Type::optional(decimal);
}

bool LibraryDecimalRegistry::isFittingSpread(const Expr *arg) const {
  const auto *array = Type::array(Type::decimalTy());

  return arg->type == array || arg->type->isError();
}

void LibraryDecimalRegistry::checkDecimalMethod(const CheckDecimalMethod &task) {
  CallExpr *call = task.call;
  MemberExpr *member = task.member;
  auto &tasks = this->state.walk.tasks;
  tasks.pushChecked(call);
  const auto *method = DecimalMethodTable::find(member->name);
  if (method == nullptr) {
    auto use = ErrorTextDecimalMethod::replacementFor(member->name);
    auto message = ErrorTextDecimalMethod::noMember(member->name, use);
    this->state.diagnostics.error(member->loc(), message);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  if (method->builtin == Builtin::DecDecimalPlaces && !call->args.empty()) {
    auto message = ErrorTextDecimalMethod::places();
    this->state.diagnostics.error(member->loc(), message);

    return tasks.pushTask(WalkArgsLoosely{.call = call});
  }
  auto arity = this->state.library.common.checkArity(call, method->minArgs, method->maxArgs);
  if (arity.isErr()) {
    return this->state.diagnostics.error(member->loc(), arity.error().message);
  }
  tasks.pushTask(FinishDecimalMethod{.call = call, .method = method});
  for (size_t index = call->args.size(); index > 0; --index) {
    tasks.pushTask(DecimalArgSlot{
        .slot = &call->args[index - 1],
        .kind = method->args[index - 1],
        .builtin = method->builtin,
    });
  }
}

void LibraryDecimalRegistry::finishDecimalMethod(const FinishDecimalMethod &task) {
  CallExpr *call = task.call;
  const DecimalMethodInfo *method = task.method;
  if (method->builtin == Builtin::DecDiv) {
    this->walkDecimalDivisor(BinaryOp::Div, call->args[0]);
  }
  call->target = BuiltinCall{.builtin = method->builtin};
  call->type = this->decimalResultType(method->result);
}

void LibraryDecimalRegistry::decimalArgSlot(const DecimalArgSlot &task) {
  auto &tasks = this->state.walk.tasks;
  bool isPositive = task.builtin == Builtin::DecDiv || task.builtin == Builtin::DecToExponential;
  switch (task.kind) {
    case DecimalArg::DECIMAL:
      return tasks.pushTask(CheckArgSlot{.slot = task.slot, .to = Type::decimalTy()});
    case DecimalArg::PLACES:
      return this->pushDecimalCount(task.slot, ErrorTextDecimalCount::places(),
                                    isPositive ? 0 : -kCountLimit, kCountLimit);
    case DecimalArg::DIGITS:
      return this->pushDecimalCount(task.slot, ErrorTextDecimalCount::digits(), 1, kCountLimit);
    case DecimalArg::BASE:
      return this->pushDecimalCount(task.slot, ErrorTextDecimalCount::base(), kLowestBase,
                                    kHighestBase);
    case DecimalArg::POWER:
      return this->pushDecimalCount(task.slot, ErrorTextDecimalCount::power(), -kSafeIntegerLimit,
                                    kSafeIntegerLimit);
    case DecimalArg::NONE:
    case DecimalArg::ROUNDING:
      tasks.pushTask(AfterRoundingMode{.slot = task.slot});
      return tasks.pushTask(CheckArgSlot{.slot = task.slot, .to = Type::string()});
  }
}

void LibraryDecimalRegistry::pushDecimalCount(Expr **slot, std::string what, int64_t low,
                                              int64_t high) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(
      AfterDecimalCount{.slot = slot, .what = std::move(what), .low = low, .high = high});

  return tasks.pushTask(CheckExprSlot{.slot = slot, .expected = Type::intTy()});
}

void LibraryDecimalRegistry::afterDecimalCount(const AfterDecimalCount &task) {
  const Expr *arg = *task.slot;
  const Type *type = arg->type;
  if (type->isError()) {
    return;
  }
  if (!type->isInteger()) {
    auto message = ErrorTextDecimalCount::notInt(task.what, type->name(), type->isOptional());

    return this->state.diagnostics.error(arg->loc(), message);
  }
  auto between = this->state.flow.proofs.requireBetween(arg, task.low, task.high, task.what);
  bool isShown =
      between.isErr() && between.error().code != FlowProofsRequireBetweenErrorCode::EXEMPT;
  if (isShown) {
    this->state.diagnostics.error(arg->loc(), between.error().message);
  }

  return this->state.walk.tasks.pushTask(ConvertSlot{.slot = task.slot, .to = Type::intTy()});
}

void LibraryDecimalRegistry::afterRoundingMode(const AfterRoundingMode &task) {
  const Expr *checked = *task.slot;
  if (checked->type->isError()) {
    return;
  }
  const auto *literal = dyn_cast<StringLitExpr>(checked);
  if (literal != nullptr) {
    auto mode = this->checkRoundingModeText(literal->value);
    if (mode.isErr()) {
      this->state.diagnostics.error(checked->loc(), mode.error().message);
    }

    return;
  }
  auto modes = this->state.library.common.knownTexts(checked);
  if (!modes.has_value()) {
    auto message = ErrorTextRoundingMode::notKnown();

    return this->state.diagnostics.error(checked->loc(), message);
  }

  return this->walkKnownModes(modes.value(), checked);
}

std::string LibraryDecimalRegistry::operandTypesText(llvm::StringRef op, const Type *lhs,
                                                     const Type *rhs) const {
  bool isUndefined =
      lhs->isOptional() || rhs->isOptional() || lhs->isUndefined() || rhs->isUndefined();

  return ErrorTextOperands::types(op, lhs->name(), rhs->name(), isUndefined);
}

LibraryDecimalRegistry::ResultRoundingMode
LibraryDecimalRegistry::checkRoundingModeText(const std::u16string &text) const {
  int mode = DecimalLiteral::roundingMode(text);
  if (mode < 0) {
    auto written = ToUtf8::toUtf8(text);
    auto message = ErrorTextRoundingMode::unknown(written);

    return err(LibraryDecimalRegistryRoundingModeError{
        .code = LibraryDecimalRegistryRoundingModeErrorCode::UNKNOWN_MODE,
        .message = message,
    });
  }

  return ok(mode);
}

LibraryDecimalRegistry::ResultLiteralRange
LibraryDecimalRegistry::checkLiteralRange(const Expr *literal) const {
  auto value = DecimalLiteral::fromExpr(literal);
  if (!value.has_value()) {
    auto message = ErrorTextDecimalLit::unreadable();

    return err(LibraryDecimalRegistryLiteralRangeError{
        .code = LibraryDecimalRegistryLiteralRangeErrorCode::UNREADABLE,
        .message = message,
    });
  }
  if (!value->isZero() && value->top() > kDecimalMaxExp) {
    auto message = ErrorTextDecimalLit::outOfRange(kDecimalMaxExp + 1);

    return err(LibraryDecimalRegistryLiteralRangeError{
        .code = LibraryDecimalRegistryLiteralRangeErrorCode::OUT_OF_RANGE,
        .message = message,
    });
  }

  return ok(value.value());
}

std::pair<llvm::StringRef, llvm::StringRef>
LibraryDecimalRegistry::sidesOf(const BinaryExpr *binary) const {
  llvm::StringRef source = binary->source;
  const char *at = binary->loc().getPointer();
  size_t length = std::strlen(Operators::opSpelling(binary->op));
  if (source.empty() || at < source.begin() || at + length > source.end()) {
    return {};
  }

  return {llvm::StringRef(source.begin(), static_cast<size_t>(at - source.begin())).trim(),
          llvm::StringRef(at + length, static_cast<size_t>(source.end() - at - length)).trim()};
}

bool LibraryDecimalRegistry::isLiteralTree(const Expr *expr) const {
  for (const Expr *node : ExprParts::operatorTree(expr)) {
    if (!this->isLiteralNode(node)) {
      return false;
    }
  }

  return true;
}

bool LibraryDecimalRegistry::isLiteralNode(const Expr *expr) const {
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

bool LibraryDecimalRegistry::isArithmeticOperator(BinaryOp op) const {
  return op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul || op == BinaryOp::Div ||
         op == BinaryOp::Rem || op == BinaryOp::Pow;
}

bool LibraryDecimalRegistry::isFittingLiteral(const Expr *expr, const Type *type) const {
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

const Type *LibraryDecimalRegistry::literalHint(const Expr *other, const Type *hint) const {
  return other->type != nullptr && other->type->nonOptional()->isDecimal() ? Type::decimalTy()
                                                                           : hint;
}

bool LibraryDecimalRegistry::isComparedLiteral(const Expr *expr) const {
  const auto *unary = dyn_cast<UnaryExpr>(expr);
  bool isNegated = unary != nullptr && unary->op == UnaryOp::Neg;
  const Expr *operand = isNegated ? unary->operand : expr;
  const auto *member = dyn_cast<MemberExpr>(operand);
  const auto *owner = member != nullptr ? dyn_cast<NameExpr>(member->object) : nullptr;

  return this->isLiteralTree(operand) || isa<StringLitExpr>(operand) || isa<BoolLitExpr>(operand) ||
         (owner != nullptr && this->state.walk.scopes.findName(owner->name).enumDecl() != nullptr);
}

const Type *LibraryDecimalRegistry::comparisonHint(const Expr *other, const Expr *self) const {
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

bool LibraryDecimalRegistry::isDecimalText(const Type *type) const {
  if (type->isOptional()) {
    return false;
  }
  const Type *text = type->nonOptional();
  auto isStringLiteral = std::bind_front(&LibraryDecimalRegistry::isStringLiteralType, this);

  return text->isString() || text->isStringLiteral() ||
         (text->isUnion() && llvm::all_of(text->members(), isStringLiteral));
}

bool LibraryDecimalRegistry::isStringLiteralType(const Type *type) const {
  return type->isStringLiteral();
}

bool LibraryDecimalRegistry::reportBadDecimalText(llvm::ArrayRef<std::string> texts,
                                                  const Expr *arg) {
  for (const std::string &text : texts) {
    if (!this->isDecimalNumberText(text)) {
      auto message = ErrorTextDecimalOf::notNumber(text);
      this->state.diagnostics.error(arg->loc(), message);
      return true;
    }
  }

  return false;
}

bool LibraryDecimalRegistry::isDecimalNumberText(const std::string &text) const {
  auto units = llvm::SmallVector<llvm::UTF16, 32>();
  llvm::convertUTF8ToUTF16String(text, units);
  const auto *data = reinterpret_cast<const uint16_t *>(units.data());

  return tz_decimal_text_ok(data, units.size());
}

void LibraryDecimalRegistry::walkKnownModes(llvm::ArrayRef<std::string> modes, const Expr *arg) {
  for (const std::string &mode : modes) {
    std::u16string text(mode.begin(), mode.end());
    auto known = this->checkRoundingModeText(text);
    if (known.isErr()) {
      this->state.diagnostics.error(arg->loc(), known.error().message);
      return;
    }
  }
}

const Type *LibraryDecimalRegistry::decimalResultType(DecimalResult result) const {
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
