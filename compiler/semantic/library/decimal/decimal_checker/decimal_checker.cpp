#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/binary_checker/binary_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/semantic/declare/prelude/prelude.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/names/scopes/scopes.h"
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
  std::optional<DecimalLiteral> value = DecimalChecker::decimalLiteral(lit);
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

DecimalChecker::DecimalChecker(SemanticState &owner)
    : state(owner) {}

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
  switch (expr->kind()) {
  case Expr::Kind::IntLit:
  case Expr::Kind::FloatLit:
    return true;
  case Expr::Kind::Unary: {
    auto *unary = cast<UnaryExpr>(expr);
    return (unary->op == UnaryOp::Neg || unary->op == UnaryOp::Plus) &&
           DecimalChecker::isLiteralTree(unary->operand);
  }
  case Expr::Kind::Binary: {
    auto *bin = cast<BinaryExpr>(expr);
    switch (bin->op) {
    case BinaryOp::Add:
    case BinaryOp::Sub:
    case BinaryOp::Mul:
    case BinaryOp::Div:
    case BinaryOp::Rem:
    case BinaryOp::Pow:
      return DecimalChecker::isLiteralTree(bin->lhs) && DecimalChecker::isLiteralTree(bin->rhs);
    default:
      return false;
    }
  }
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
           (owner && Scopes(this->state).lookup(owner->name).enumDecl());
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
    bin->rhs = ExprChecker(this->state).checkExpr(bin->rhs);
    bin->lhs = ExprChecker(this->state).checkExpr(bin->lhs, hint(bin->rhs, bin->lhs));
    return;
  }
  bin->lhs = ExprChecker(this->state).checkExpr(bin->lhs);
  bin->rhs = ExprChecker(this->state)
                 .checkExpr(bin->rhs, literal(bin->rhs) ? hint(bin->lhs, bin->rhs) : nullptr);
}

std::optional<DecimalLiteral> DecimalChecker::decimalLiteral(const Expr *expr) {
  if (auto *unary = dyn_cast<UnaryExpr>(expr); unary && unary->op == UnaryOp::Neg) {
    expr = unary->operand;
  }
  if (auto *lit = dyn_cast<IntLitExpr>(expr)) {
    return lit->text.empty() ? DecimalLiteral::of(lit->value) : DecimalLiteral::read(lit->text);
  }
  if (auto *lit = dyn_cast<FloatLitExpr>(expr)) {
    return DecimalLiteral::read(lit->text);
  }

  return std::nullopt;
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
    bin->rhs = TypeRelations(this->state).convert(bin->rhs, Type::intTy());
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
    *side = TypeRelations(this->state).convert(*side, decimal);
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
    *side = TypeRelations(this->state).convert(*side, Type::decimalTy());
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
    assign->value = TypeRelations(this->state).convert(assign->value, Type::intTy());
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
  assign->value = TypeRelations(this->state).coerce(assign->value, Type::decimalTy());
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
  std::optional<DecimalLiteral> literal = DecimalChecker::decimalLiteral(value);
  bool zero = literal ? literal->isZero() : ConstantFolder(this->state).intConstant(value) == 0;
  if (zero) {
    std::string message = ErrorTextDecimalPower::divZero();
    this->state.getDiagnostics().error(divisor->loc(), message);
  }
}

Expr *DecimalChecker::checkDecimalCall(CallExpr *call, llvm::SMLoc loc) {
  call->type = Type::error();
  ArgumentChecker::ResultArityMatches arity = ArgumentChecker(this->state).checkArity(call, 1, 1);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
    return call;
  }
  Expr *&arg = call->args[0];
  bool literal = DecimalChecker::decimalLiteral(arg).has_value();
  arg = ExprChecker(this->state).checkExpr(arg, literal ? Type::decimalTy() : nullptr);
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
    arg = TypeRelations(this->state).convert(arg, Type::decimalTy());
  } else if (textual) {
    std::optional<std::vector<std::string>> texts = LiteralTypes(this->state).knownTexts(arg);
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
      arg = TypeRelations(this->state).convert(arg, Type::string());
    }
    call->target = BuiltinCall{.builtin = Builtin::DecimalOf};
    call->type =
        texts ? Type::decimalTy()
              : Type::result(Type::decimalTy(), Prelude(this->state).builtinType("DecimalError"));
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
      arg = ExprChecker(this->state).checkExpr(arg);
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
      arg = ArgumentChecker(this->state).checkArg(arg, decimal);
      someValue = true;
      continue;
    }
    arg = ExprChecker(this->state).checkExpr(arg, array);
    if (arg->type != array && !arg->type->isError()) {
      std::string message = ErrorTextDecimalStatic::spread(arg->type->name());
      this->state.getDiagnostics().error(arg->loc(), message);
      ok = false;
    }
    someValue |= IndexProofs(this->state).provesNotEmpty(arg);
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
  arg = ArgumentChecker(this->state).checkArg(arg, Type::string());
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
  std::optional<std::vector<std::string>> modes = LiteralTypes(this->state).knownTexts(arg);
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
  arg = ExprChecker(this->state).checkExpr(arg, Type::intTy());
  const Type *type = arg->type;
  if (type->isError()) {
    return arg;
  }
  if (!type->isInteger()) {
    std::string message = ErrorTextDecimalCount::notInt(what, type->name(), type->isOptional());
    this->state.getDiagnostics().error(arg->loc(), message);
    return arg;
  }
  IndexProofs::ResultBetweenProven between =
      IndexProofs(this->state).requireBetween(arg, lo, hi, what);
  if (between.isErr() && between.error().code != IndexProofsRequireBetweenErrorCode::EXEMPT) {
    this->state.getDiagnostics().error(arg->loc(), between.error().message);
  }

  return TypeRelations(this->state).convert(arg, Type::intTy());
}

Expr *DecimalChecker::checkDecimalMethod(CallExpr *call, MemberExpr *member) {
  struct Method {
    llvm::StringLiteral name;
    Builtin builtin;
    unsigned minArgs, maxArgs;
    const char *args;
    const Type *ret;
  };
  const Type *decimal = Type::decimalTy();
  const Method methods[] = {
      {"div", Builtin::DecDiv, 1, 3, "dpm", decimal},
      {"round", Builtin::DecRound, 0, 2, "pm", decimal},
      {"toFixed", Builtin::DecToFixed, 0, 2, "pm", Type::string()},
      {"toFormat", Builtin::DecToFormat, 0, 2, "pm", Type::string()},
      {"toExponential", Builtin::DecToExponential, 0, 2, "pm", Type::string()},
      {"toPrecision", Builtin::DecToPrecision, 0, 2, "sm", Type::string()},
      {"toString", Builtin::DecToString, 0, 1, "b", Type::string()},
      {"sqrt", Builtin::DecSqrt, 0, 0, "", decimal},
      {"abs", Builtin::DecAbs, 0, 0, "", decimal},
      {"isInteger", Builtin::DecIsInteger, 0, 0, "", Type::boolTy()},
      {"decimalPlaces", Builtin::DecDecimalPlaces, 0, 0, "", Type::intTy()},
      {"shiftedBy", Builtin::DecShiftedBy, 1, 1, "k", decimal},
      {"toFraction", Builtin::DecToFraction, 0, 1, "d", Type::array(decimal)},
  };
  llvm::StringRef name = member->name;
  const Method *method = nullptr;
  for (const Method &m : methods) {
    if (m.name == name) {
      method = &m;
    }
  }
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args) {
      arg = ExprChecker(this->state).checkExpr(arg);
    }
  };
  if (!method) {
    llvm::StringRef use;
    for (const Replacement &r : kReplacements) {
      if (r.name == name) {
        use = r.use;
      }
    }
    std::string message = ErrorTextDecimalMethod::noMember(name, use);
    this->state.getDiagnostics().error(member->loc(), message);
    checkArgsLoosely();
    return call;
  }
  if (method->builtin == Builtin::DecDecimalPlaces && !call->args.empty()) {
    std::string message = ErrorTextDecimalMethod::places();
    this->state.getDiagnostics().error(member->loc(), message);
    checkArgsLoosely();
    return call;
  }
  ArgumentChecker::ResultArityMatches arity =
      ArgumentChecker(this->state).checkArity(call, method->minArgs, method->maxArgs);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(member->loc(), arity.error().message);
    return call;
  }
  for (size_t i = 0; i < call->args.size(); ++i) {
    Expr *&arg = call->args[i];
    switch (method->args[i]) {
    case 'd':
      arg = ArgumentChecker(this->state).checkArg(arg, decimal);
      break;
    case 'p': {
      bool positive =
          method->builtin == Builtin::DecDiv || method->builtin == Builtin::DecToExponential;
      std::string places = ErrorTextDecimalCount::places();
      arg = this->checkDecimalCount(arg, places, positive ? 0 : -1000000000, 1000000000);
      break;
    }
    case 's': {
      std::string digits = ErrorTextDecimalCount::digits();
      arg = this->checkDecimalCount(arg, digits, 1, 1000000000);
      break;
    }
    case 'b': {
      std::string base = ErrorTextDecimalCount::base();
      arg = this->checkDecimalCount(arg, base, 2, 36);
      break;
    }
    case 'k': {
      std::string power = ErrorTextDecimalCount::power();
      arg = this->checkDecimalCount(arg, power, -9007199254740991, 9007199254740991);
      break;
    }
    default:
      arg = this->checkRoundingMode(arg);
      break;
    }
  }
  if (method->builtin == Builtin::DecDiv) {
    this->checkDecimalDivisor(BinaryOp::Div, call->args[0]);
  }
  call->target = BuiltinCall{.builtin = method->builtin};
  call->type = method->ret;

  return call;
}

}
