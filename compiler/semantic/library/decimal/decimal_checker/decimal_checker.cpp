#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker.h"
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

struct Replacement {
  llvm::StringLiteral name, use;
};

const Replacement kReplacements[] = {
    {"plus", "the operator: 'a + b'"},
    {"minus", "the operator: 'a - b'"},
    {"times", "the operator: 'a * b'"},
    {"multipliedBy", "the operator: 'a * b'"},
    {"dividedBy", "the operator 'a / b', or 'a.div(b, places, mode)'"},
    {"dividedToIntegerBy", "'a.div(b, 0, \"down\")'"},
    {"idiv", "'a.div(b, 0, \"down\")'"},
    {"modulo", "the operator: 'a % b'"},
    {"mod", "the operator: 'a % b'"},
    {"exponentiatedBy", "the operator: 'a ** n'"},
    {"pow", "the operator: 'a ** n'"},
    {"negated", "the operator: '-a'"},
    {"comparedTo", "the operators: 'a < b', 'a == b', 'a > b'"},
    {"isEqualTo", "the operator: 'a == b'"},
    {"eq", "the operator: 'a == b'"},
    {"isGreaterThan", "the operator: 'a > b'"},
    {"gt", "the operator: 'a > b'"},
    {"isGreaterThanOrEqualTo", "the operator: 'a >= b'"},
    {"gte", "the operator: 'a >= b'"},
    {"isLessThan", "the operator: 'a < b'"},
    {"lt", "the operator: 'a < b'"},
    {"isLessThanOrEqualTo", "the operator: 'a <= b'"},
    {"lte", "the operator: 'a <= b'"},
    {"isZero", "a comparison: 'a == 0'"},
    {"isNegative", "a comparison: 'a < 0'"},
    {"isPositive", "a comparison: 'a > 0'"},
    {"absoluteValue", "'a.abs()'"},
    {"squareRoot", "'a.sqrt()'"},
    {"integerValue", "'a.round(0, mode)'"},
    {"dp", "'a.decimalPlaces()', or 'a.round(places, mode)' to round"},
    {"precision", "'a.toPrecision(digits)'"},
    {"sd", "'a.toPrecision(digits)'"},
    {"toNumber", "'Number(a)'"},
    {"valueOf", "'String(a)'"},
    {"toJSON", "'String(a)'"},
};

}

DecimalChecker::DecimalChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->prelude = &all.prelude;
  this->exprChecker = &all.exprChecker;
  this->indexProofs = &all.indexProofs;
  this->literalTypes = &all.literalTypes;
  this->typeRelations = &all.typeRelations;
  this->constantFolder = &all.constantFolder;
  this->argumentChecker = &all.argumentChecker;
}

DecimalChecker::ResultRoundingMode DecimalChecker::roundingMode(const std::u16string &text) {
  int mode = DecimalLiteral::roundingMode(text);
  if (mode < 0) {
    std::string written = ToUtf8::toUtf8(text);
    std::string message = ErrorTextRoundingMode::unknown(written);

    return err(DecimalCheckerRoundingModeError{
        .code = DecimalCheckerRoundingModeErrorCode::UNKNOWN_MODE,
        .message = message,
    });
  }

  return ok(mode);
}

DecimalChecker::ResultLiteralRange DecimalChecker::literalRange(const Expr *lit) {
  std::optional<DecimalLiteral> value = DecimalLiteral::fromExpr(lit);
  if (!value) {
    std::string message = ErrorTextDecimalLit::unreadable();

    return err(DecimalCheckerLiteralRangeError{
        .code = DecimalCheckerLiteralRangeErrorCode::UNREADABLE,
        .message = message,
    });
  }
  if (!value->isZero() && value->top() > kDecimalMaxExp) {
    std::string message = ErrorTextDecimalLit::outOfRange(kDecimalMaxExp + 1);

    return err(DecimalCheckerLiteralRangeError{
        .code = DecimalCheckerLiteralRangeErrorCode::OUT_OF_RANGE,
        .message = message,
    });
  }

  return ok(*value);
}

std::pair<llvm::StringRef, llvm::StringRef> DecimalChecker::sidesOf(const BinaryExpr *bin) {
  llvm::StringRef source = bin->source;
  const char *at = bin->loc().getPointer();
  size_t length = std::strlen(Operators::opSpelling(bin->op));
  if (source.empty() || at < source.begin() || at + length > source.end()) {
    return {};
  }

  return {llvm::StringRef(source.begin(), static_cast<size_t>(at - source.begin())).trim(),
          llvm::StringRef(at + length, static_cast<size_t>(source.end() - at - length)).trim()};
}

bool DecimalChecker::isLiteralTree(const Expr *expr) {
  for (const Expr *node : ExprParts::operatorTree(expr)) {
    if (!DecimalChecker::isLiteralNode(node)) {
      return false;
    }
  }

  return true;
}

bool DecimalChecker::isLiteralNode(const Expr *expr) {
  if (isa<IntLitExpr>(expr) || isa<FloatLitExpr>(expr)) {
    return true;
  }
  if (auto *unary = dyn_cast<UnaryExpr>(expr)) {
    return unary->op == UnaryOp::Neg || unary->op == UnaryOp::Plus;
  }
  auto *bin = dyn_cast<BinaryExpr>(expr);
  if (bin == nullptr) {
    return false;
  }
  switch (bin->op) {
    case BinaryOp::Add:
    case BinaryOp::Sub:
    case BinaryOp::Mul:
    case BinaryOp::Div:
    case BinaryOp::Rem:
    case BinaryOp::Pow:
      return true;
    default:
      return false;
  }
}

bool DecimalChecker::fitsLiteral(const Expr *expr, const Type *type) {
  bool negated = false;
  if (auto *unary = dyn_cast<UnaryExpr>(expr); unary && unary->op == UnaryOp::Neg) {
    expr = unary->operand;
    negated = true;
  }
  auto *lit = dyn_cast<IntLitExpr>(expr);
  if (!lit) {
    return false;
  }
  if (negated) {
    return lit->value == 0 ||
           (!type->isUnsigned() && lit->value <= uint64_t(0) - uint64_t(type->intMin()));
  }

  return lit->value <= type->intMax();
}

const Type *DecimalChecker::literalHint(const Expr *other, const Type *hint) {
  return other->type && other->type->nonOptional()->isDecimal() ? Type::decimalTy() : hint;
}

void DecimalChecker::checkComparisonOperands(BinaryExpr *bin) {
  auto literal = [&](const Expr *e) {
    if (auto *unary = dyn_cast<UnaryExpr>(e); unary && unary->op == UnaryOp::Neg) {
      e = unary->operand;
    }

    auto *member = dyn_cast<MemberExpr>(e);
    auto *owner = member ? dyn_cast<NameExpr>(member->object) : nullptr;

    return DecimalChecker::isLiteralTree(e) || isa<StringLitExpr>(e) || isa<BoolLitExpr>(e) ||
           (owner && this->scopes->lookup(owner->name).enumDecl());
  };
  auto hint = [&](const Expr *other, const Expr *self) {
    const Type *type = other->type ? other->type->nonOptional() : nullptr;
    if (type && (type->isLiteral() || type->isUnion())) {
      return other->type;
    }
    if (type && type->isNumeric() && (type->isFloat() || DecimalChecker::fitsLiteral(self, type))) {
      return type;
    }

    return DecimalChecker::literalHint(other, nullptr);
  };
  if (literal(bin->lhs) && !literal(bin->rhs)) {
    bin->rhs = this->exprChecker->checkExpr(bin->rhs);
    bin->lhs = this->exprChecker->checkExpr(bin->lhs, hint(bin->rhs, bin->lhs));
    return;
  }
  bin->lhs = this->exprChecker->checkExpr(bin->lhs);
  bin->rhs = this->exprChecker->checkExpr(bin->rhs,
                                          literal(bin->rhs) ? hint(bin->lhs, bin->rhs) : nullptr);
}

void DecimalChecker::checkDecimalLiteralRange(const Expr *lit) {
  ResultLiteralRange range = DecimalChecker::literalRange(lit);
  if (range.isErr()) {
    this->state.getDiagnostics().error(lit->loc(), range.error().message);
  }
}

void DecimalChecker::reportDecimalMix(const BinaryExpr *bin) {
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
  bool leftDecimal = l->nonOptional()->isDecimal();
  const Type *other = leftDecimal ? r : l;
  if (other->isInteger()) {
    auto [lhs, rhs] = DecimalChecker::sidesOf(bin);
    std::string message = ErrorTextDecimalMix::integer(Operators::opSpelling(bin->op), l->name(),
                                                       r->name(), lhs, rhs, leftDecimal);
    this->state.getDiagnostics().error(bin->loc(), message);
    return;
  }
  if (!other->nonOptional()->isFloat()) {
    std::string message = BinaryChecker::operandTypesText(Operators::opSpelling(bin->op), l, r);
    this->state.getDiagnostics().error(bin->loc(), message);
    return;
  }
  auto [lhs, rhs] = DecimalChecker::sidesOf(bin);
  std::string message =
      ErrorTextDecimalMix::inexact(Operators::opSpelling(bin->op), l->name(), r->name(), lhs, rhs,
                                   leftDecimal, other->isFloat());
  this->state.getDiagnostics().error(bin->loc(), message);
}

Expr *DecimalChecker::checkDecimalArithmetic(BinaryExpr *bin) {
  const Type *decimal = Type::decimalTy();
  const Type *l = bin->lhs->type;
  const Type *r = bin->rhs->type;
  bin->type = Type::error();
  if (bin->op == BinaryOp::Pow) {
    if (!l->isDecimal()) {
      std::string message = BinaryChecker::operandTypesText("**", l, r);
      this->state.getDiagnostics().error(bin->loc(), message);
      return bin;
    }
    if (!r->isInteger()) {
      std::string message = ErrorTextDecimalPower::exponent(r);
      this->state.getDiagnostics().error(bin->rhs->loc(), message);
      return bin;
    }
    bin->rhs = this->typeRelations->convert(bin->rhs, Type::intTy());
    bin->type = decimal;
    return bin;
  }
  for (Expr **side : {&bin->lhs, &bin->rhs}) {
    const Type *type = (*side)->type;
    if (type->isDecimal()) {
      continue;
    }
    if (!type->isInteger() || !TypeRelations::isIntLiteralTree(*side)) {
      this->reportDecimalMix(bin);
      return bin;
    }
    *side = this->typeRelations->convert(*side, decimal);
  }
  this->checkDecimalDivisor(bin->op, bin->rhs);
  bin->type = decimal;

  return bin;
}

Expr *DecimalChecker::checkDecimalComparison(BinaryExpr *bin) {
  bin->type = Type::boolTy();
  for (Expr **side : {&bin->lhs, &bin->rhs}) {
    const Type *type = (*side)->type;
    if (type->isDecimal()) {
      continue;
    }
    if (!type->isInteger() || !TypeRelations::isIntLiteralTree(*side)) {
      this->reportDecimalMix(bin);
      return bin;
    }
    *side = this->typeRelations->convert(*side, Type::decimalTy());
  }

  return bin;
}

void DecimalChecker::checkDecimalAssign(AssignExpr *assign, BinaryOp op) {
  const Type *valueType = assign->value->type;
  if (op == BinaryOp::Pow) {
    if (!valueType->isInteger()) {
      std::string message = ErrorTextDecimalPower::exponent(valueType);
      this->state.getDiagnostics().error(assign->value->loc(), message);
      return;
    }
    assign->value = this->typeRelations->convert(assign->value, Type::intTy());
    return;
  }
  bool arithmetic = op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul ||
                    op == BinaryOp::Div || op == BinaryOp::Rem;
  if (!arithmetic) {
    std::string spelling = std::string(Operators::opSpelling(op)) + "=";
    std::string message = BinaryChecker::operandTypesText(spelling, assign->type, valueType);
    this->state.getDiagnostics().error(assign->loc(), message);
    return;
  }
  assign->value = this->typeRelations->coerce(assign->value, Type::decimalTy());
  this->checkDecimalDivisor(op, assign->value);
}

void DecimalChecker::checkDecimalDivisor(BinaryOp op, const Expr *divisor) {
  if (op != BinaryOp::Div && op != BinaryOp::Rem) {
    return;
  }
  const Expr *value = divisor;
  for (auto *conv = dyn_cast<CastExpr>(value); conv && conv->isImplicit();
       conv = dyn_cast<CastExpr>(value)) {
    value = conv->operand;
  }
  std::optional<DecimalLiteral> literal = DecimalLiteral::fromExpr(value);
  bool zero = literal ? literal->isZero() : this->constantFolder->intConstant(value) == 0;
  if (zero) {
    std::string message = ErrorTextDecimalPower::divZero();
    this->state.getDiagnostics().error(divisor->loc(), message);
  }
}

Expr *DecimalChecker::checkDecimalCall(CallExpr *call, llvm::SMLoc loc) {
  call->type = Type::error();
  ArgumentChecker::ResultArityMatches arity = this->argumentChecker->checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  Expr *&arg = call->args[0];
  bool literal = DecimalLiteral::fromExpr(arg).has_value();
  arg = this->exprChecker->checkExpr(arg, literal ? Type::decimalTy() : nullptr);
  const Type *type = arg->type;
  if (type->isError()) {
    return call;
  }
  const Type *text = type->nonOptional();
  bool textual =
      !type->isOptional() && (text->isString() || text->isStringLiteral() ||
                              (text->isUnion() && llvm::all_of(text->members(), [](const Type *m) {
                                 return m->isStringLiteral();
                               })));
  if (type->isInteger() || type->isDecimal()) {
    arg = this->typeRelations->convert(arg, Type::decimalTy());
  } else if (textual) {
    std::optional<std::vector<std::string>> texts = this->literalTypes->knownTexts(arg);
    for (const std::string &t : texts ? *texts : std::vector<std::string>()) {
      llvm::SmallVector<llvm::UTF16, 32> units;
      llvm::convertUTF8ToUTF16String(t, units);
      if (!tz_decimal_text_ok(reinterpret_cast<const uint16_t *>(units.data()), units.size())) {
        std::string message = ErrorTextDecimalOf::notNumber(t);
        this->state.getDiagnostics().error(arg->loc(), message);
        return call;
      }
    }
    if (!text->isString()) {
      arg = this->typeRelations->convert(arg, Type::string());
    }
    call->target = BuiltinCall{.builtin = Builtin::DecimalOf};
    call->type = texts
                     ? Type::decimalTy()
                     : Type::result(Type::decimalTy(), this->prelude->builtinType("DecimalError"));
    return call;
  } else if (!type->isFloat()) {
    std::string message = ErrorTextDecimalOf::takes(type->name(), type->isOptional());
    this->state.getDiagnostics().error(arg->loc(), message);
    return call;
  }
  call->target = BuiltinCall{.builtin = Builtin::DecimalOf};
  call->type = Type::decimalTy();

  return call;
}

Expr *DecimalChecker::checkDecimalStatic(CallExpr *call, MemberExpr *member) {
  llvm::StringRef name = member->name;
  call->type = Type::error();
  Builtin builtin = name == "max"   ? Builtin::DecMax
                    : name == "min" ? Builtin::DecMin
                    : name == "sum" ? Builtin::DecSum
                                    : Builtin::None;
  if (builtin == Builtin::None) {
    std::string message = ErrorTextDecimalStatic::noMember(name);
    this->state.getDiagnostics().error(member->loc(), message);
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return call;
  }
  const Type *decimal = Type::decimalTy();
  const Type *array = Type::array(decimal);
  bool someValue = false;
  bool ok = true;
  for (size_t i = 0; i < call->args.size(); ++i) {
    Expr *&arg = call->args[i];
    if (!call->spreads(i)) {
      arg = this->argumentChecker->checkArg(arg, decimal);
      someValue = true;
      continue;
    }
    arg = this->exprChecker->checkExpr(arg, array);
    if (arg->type != array && !arg->type->isError()) {
      std::string message = ErrorTextDecimalStatic::spread(arg->type->name());
      this->state.getDiagnostics().error(arg->loc(), message);
      ok = false;
    }
    someValue |= this->indexProofs->provesNotEmpty(arg);
  }
  if (!ok) {
    return call;
  }
  if (call->args.empty() && builtin != Builtin::DecSum) {
    std::string message = ErrorTextDecimalStatic::noValue(name);
    this->state.getDiagnostics().error(member->loc(), message);
    return call;
  }
  call->target = BuiltinCall{.builtin = builtin};
  call->type = someValue || builtin == Builtin::DecSum ? decimal : Type::optional(decimal);

  return call;
}

Expr *DecimalChecker::checkRoundingMode(Expr *arg) {
  arg = this->argumentChecker->checkArg(arg, Type::string());
  if (arg->type->isError()) {
    return arg;
  }
  if (auto *lit = dyn_cast<StringLitExpr>(arg)) {
    ResultRoundingMode mode = DecimalChecker::roundingMode(lit->value);
    if (mode.isErr()) {
      this->state.getDiagnostics().error(arg->loc(), mode.error().message);
    }
    return arg;
  }
  std::optional<std::vector<std::string>> modes = this->literalTypes->knownTexts(arg);
  if (!modes) {
    std::string message = ErrorTextRoundingMode::notKnown();
    this->state.getDiagnostics().error(arg->loc(), message);
    return arg;
  }
  for (const std::string &mode : *modes) {
    std::u16string text(mode.begin(), mode.end());
    ResultRoundingMode known = DecimalChecker::roundingMode(text);
    if (known.isErr()) {
      this->state.getDiagnostics().error(arg->loc(), known.error().message);
      break;
    }
  }

  return arg;
}

Expr *DecimalChecker::checkDecimalCount(Expr *arg, llvm::StringRef what, int64_t lo, int64_t hi) {
  arg = this->exprChecker->checkExpr(arg, Type::intTy());
  const Type *type = arg->type;
  if (type->isError()) {
    return arg;
  }
  if (!type->isInteger()) {
    std::string message = ErrorTextDecimalCount::notInt(what, type->name(), type->isOptional());
    this->state.getDiagnostics().error(arg->loc(), message);
    return arg;
  }
  IndexProofs::ResultBetweenProven between = this->indexProofs->requireBetween(arg, lo, hi, what);
  if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
    this->state.getDiagnostics().error(arg->loc(), between.error().message);
  }

  return this->typeRelations->convert(arg, Type::intTy());
}

Expr *DecimalChecker::checkDecimalMethod(CallExpr *call, MemberExpr *member) {
  const DecimalMethodInfo *method = DecimalMethodTable::find(member->name);
  if (method == nullptr) {
    return this->reportDecimalMember(call, member);
  }
  if (method->builtin == Builtin::DecDecimalPlaces && !call->args.empty()) {
    std::string message = ErrorTextDecimalMethod::places();
    this->state.getDiagnostics().error(member->loc(), message);
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return call;
  }
  ArgumentChecker::ResultArityMatches arity =
      this->argumentChecker->checkArity(call, method->minArgs, method->maxArgs);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  for (size_t i = 0; i < call->args.size(); ++i) {
    call->args[i] = this->checkDecimalArg(call->args[i], method->args[i], method->builtin);
  }
  if (method->builtin == Builtin::DecDiv) {
    this->checkDecimalDivisor(BinaryOp::Div, call->args[0]);
  }
  call->target = BuiltinCall{.builtin = method->builtin};
  call->type = DecimalChecker::decimalResultType(method->result);

  return call;
}

Expr *DecimalChecker::reportDecimalMember(CallExpr *call, MemberExpr *member) {
  llvm::StringRef use;
  for (const Replacement &r : kReplacements) {
    if (r.name == member->name) {
      use = r.use;
    }
  }
  std::string message = ErrorTextDecimalMethod::noMember(member->name, use);
  this->state.getDiagnostics().error(member->loc(), message);
  for (Expr *&arg : call->args) {
    arg = this->exprChecker->checkExpr(arg);
  }

  return call;
}

Expr *DecimalChecker::checkDecimalArg(Expr *arg, DecimalArg kind, Builtin builtin) {
  switch (kind) {
    case DecimalArg::DECIMAL:
      return this->argumentChecker->checkArg(arg, Type::decimalTy());
    case DecimalArg::PLACES: {
      bool positive = builtin == Builtin::DecDiv || builtin == Builtin::DecToExponential;
      std::string places = ErrorTextDecimalCount::places();
      return this->checkDecimalCount(arg, places, positive ? 0 : -1000000000, 1000000000);
    }
    case DecimalArg::DIGITS: {
      std::string digits = ErrorTextDecimalCount::digits();
      return this->checkDecimalCount(arg, digits, 1, 1000000000);
    }
    case DecimalArg::BASE: {
      std::string base = ErrorTextDecimalCount::base();
      return this->checkDecimalCount(arg, base, 2, 36);
    }
    case DecimalArg::POWER: {
      std::string power = ErrorTextDecimalCount::power();
      return this->checkDecimalCount(arg, power, -9007199254740991, 9007199254740991);
    }
    case DecimalArg::NONE:
    case DecimalArg::ROUNDING:
      return this->checkRoundingMode(arg);
  }

  return this->checkRoundingMode(arg);
}

const Type *DecimalChecker::decimalResultType(DecimalResult result) {
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
