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
  this->binaryChecker = &all.binaryChecker;
  this->constantFolder = &all.constantFolder;
  this->argumentChecker = &all.argumentChecker;
}

DecimalChecker::ResultRoundingMode DecimalChecker::roundingMode(const std::u16string &text) const {
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

DecimalChecker::ResultLiteralRange DecimalChecker::literalRange(const Expr *literal) const {
  auto value = DecimalLiteral::fromExpr(literal);
  if (!value) {
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

  return ok(*value);
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
  auto *unary = dyn_cast<UnaryExpr>(expr);
  if (unary != nullptr) {
    return unary->op == UnaryOp::Neg || unary->op == UnaryOp::Plus;
  }
  auto *binary = dyn_cast<BinaryExpr>(expr);
  if (binary == nullptr) {
    return false;
  }
  switch (binary->op) {
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

bool DecimalChecker::fitsLiteral(const Expr *expr, const Type *type) const {
  bool isNegated = false;
  auto *unary = dyn_cast<UnaryExpr>(expr);
  if (unary != nullptr && unary->op == UnaryOp::Neg) {
    expr = unary->operand;
    isNegated = true;
  }
  auto *literal = dyn_cast<IntLitExpr>(expr);
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
  auto literal = [&](const Expr *expr) {
    auto *unary = dyn_cast<UnaryExpr>(expr);
    if (unary != nullptr && unary->op == UnaryOp::Neg) {
      expr = unary->operand;
    }

    auto *member = dyn_cast<MemberExpr>(expr);
    auto *owner = member != nullptr ? dyn_cast<NameExpr>(member->object) : nullptr;

    return this->isLiteralTree(expr) || isa<StringLitExpr>(expr) || isa<BoolLitExpr>(expr) ||
           (owner != nullptr && this->scopes->lookup(owner->name).enumDecl() != nullptr);
  };
  auto hint = [&](const Expr *other, const Expr *self) {
    const Type *type = other->type != nullptr ? other->type->nonOptional() : nullptr;
    if (type != nullptr && (type->isLiteral() || type->isUnion())) {
      return other->type;
    }
    if (type != nullptr && type->isNumeric() &&
        (type->isFloat() || this->fitsLiteral(self, type))) {
      return type;
    }

    return this->literalHint(other, nullptr);
  };
  if (literal(binary->lhs) && !literal(binary->rhs)) {
    binary->rhs = this->exprChecker->checkExpr(binary->rhs);
    binary->lhs = this->exprChecker->checkExpr(binary->lhs, hint(binary->rhs, binary->lhs));
    return;
  }
  binary->lhs = this->exprChecker->checkExpr(binary->lhs);
  binary->rhs = this->exprChecker->checkExpr(
      binary->rhs, literal(binary->rhs) ? hint(binary->lhs, binary->rhs) : nullptr);
}

DecimalChecker::ResultDecimalLiteralChecked
DecimalChecker::checkDecimalLiteralRange(const Expr *literal) {
  auto range = this->literalRange(literal);
  if (range.isErr()) {
    return err(range.error());
  }

  return ok();
}

void DecimalChecker::reportDecimalMix(const BinaryExpr *binary) {
  const Type *l = binary->lhs->type;
  const Type *r = binary->rhs->type;
  bool isLeftDecimal = l->nonOptional()->isDecimal();
  const Type *other = isLeftDecimal ? r : l;
  if (other->isInteger()) {
    auto [lhs, rhs] = this->sidesOf(binary);
    auto message = ErrorTextDecimalMix::integer(Operators::opSpelling(binary->op), l->name(),
                                                r->name(), lhs, rhs, isLeftDecimal);
    this->state.getDiagnostics().error(binary->loc(), message);
    return;
  }
  if (!other->nonOptional()->isFloat()) {
    auto message = this->binaryChecker->operandTypesText(Operators::opSpelling(binary->op), l, r);
    this->state.getDiagnostics().error(binary->loc(), message);
    return;
  }
  auto [lhs, rhs] = this->sidesOf(binary);
  auto message = ErrorTextDecimalMix::inexact(Operators::opSpelling(binary->op), l->name(),
                                              r->name(), lhs, rhs, isLeftDecimal, other->isFloat());
  this->state.getDiagnostics().error(binary->loc(), message);
}

Expr *DecimalChecker::checkDecimalArithmetic(BinaryExpr *binary) {
  const auto *decimal = Type::decimalTy();
  const Type *l = binary->lhs->type;
  const Type *r = binary->rhs->type;
  binary->type = Type::error();
  if (binary->op == BinaryOp::Pow) {
    if (!l->isDecimal()) {
      auto message = this->binaryChecker->operandTypesText("**", l, r);
      this->state.getDiagnostics().error(binary->loc(), message);
      return binary;
    }
    if (!r->isInteger()) {
      auto message = ErrorTextDecimalPower::exponent(r);
      this->state.getDiagnostics().error(binary->rhs->loc(), message);
      return binary;
    }
    binary->rhs = this->typeRelations->convert(binary->rhs, Type::intTy());
    binary->type = decimal;
    return binary;
  }
  for (Expr **side : {&binary->lhs, &binary->rhs}) {
    const Type *type = (*side)->type;
    if (type->isDecimal()) {
      continue;
    }
    if (!type->isInteger() || !this->typeRelations->isIntLiteralTree(*side)) {
      this->reportDecimalMix(binary);
      return binary;
    }
    *side = this->typeRelations->convert(*side, decimal);
  }
  this->walkDecimalDivisor(binary->op, binary->rhs);
  binary->type = decimal;

  return binary;
}

Expr *DecimalChecker::checkDecimalComparison(BinaryExpr *binary) {
  binary->type = Type::boolTy();
  for (Expr **side : {&binary->lhs, &binary->rhs}) {
    const Type *type = (*side)->type;
    if (type->isDecimal()) {
      continue;
    }
    if (!type->isInteger() || !this->typeRelations->isIntLiteralTree(*side)) {
      this->reportDecimalMix(binary);
      return binary;
    }
    *side = this->typeRelations->convert(*side, Type::decimalTy());
  }

  return binary;
}

void DecimalChecker::walkDecimalAssign(AssignExpr *assign, BinaryOp op) {
  const Type *valueType = assign->value->type;
  if (op == BinaryOp::Pow) {
    if (!valueType->isInteger()) {
      auto message = ErrorTextDecimalPower::exponent(valueType);
      this->state.getDiagnostics().error(assign->value->loc(), message);
      return;
    }
    assign->value = this->typeRelations->convert(assign->value, Type::intTy());
    return;
  }
  bool isArithmetic = op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul ||
                      op == BinaryOp::Div || op == BinaryOp::Rem;
  if (!isArithmetic) {
    std::string spelling = std::string(Operators::opSpelling(op)) + "=";
    auto message = this->binaryChecker->operandTypesText(spelling, assign->type, valueType);
    this->state.getDiagnostics().error(assign->loc(), message);
    return;
  }
  assign->value = this->typeRelations->coerce(assign->value, Type::decimalTy());
  this->walkDecimalDivisor(op, assign->value);
}

void DecimalChecker::walkDecimalDivisor(BinaryOp op, const Expr *divisor) {
  if (op != BinaryOp::Div && op != BinaryOp::Rem) {
    return;
  }
  const Expr *value = divisor;
  for (auto *conversion = dyn_cast<CastExpr>(value);
       conversion != nullptr && conversion->isImplicit(); conversion = dyn_cast<CastExpr>(value)) {
    value = conversion->operand;
  }
  auto literal = DecimalLiteral::fromExpr(value);
  bool isZero = literal ? literal->isZero() : this->constantFolder->intConstant(value) == 0;
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
  arg = this->exprChecker->checkExpr(arg, isLiteral ? Type::decimalTy() : nullptr);
  const Type *type = arg->type;
  if (type->isError()) {
    return call;
  }
  const Type *text = type->nonOptional();
  bool isTextual = !type->isOptional() &&
                   (text->isString() || text->isStringLiteral() ||
                    (text->isUnion() && llvm::all_of(text->members(), [](const Type *member) {
                       return member->isStringLiteral();
                     })));
  if (type->isInteger() || type->isDecimal()) {
    arg = this->typeRelations->convert(arg, Type::decimalTy());
  } else if (isTextual) {
    auto texts = this->literalTypes->knownTexts(arg);
    for (const std::string &t : texts ? *texts : std::vector<std::string>()) {
      auto units = llvm::SmallVector<llvm::UTF16, 32>();
      llvm::convertUTF8ToUTF16String(t, units);
      if (!tz_decimal_text_ok(reinterpret_cast<const uint16_t *>(units.data()), units.size())) {
        auto message = ErrorTextDecimalOf::notNumber(t);
        this->state.getDiagnostics().error(arg->loc(), message);
        return call;
      }
    }
    if (!text->isString()) {
      arg = this->typeRelations->convert(arg, Type::string());
    }
    call->target = BuiltinCall{.builtin = Builtin::DecimalOf};
    call->type = texts ? Type::decimalTy()
                       : Type::result(Type::decimalTy(),
                                      this->prelude->builtinType(PreludeType::DECIMAL_ERROR));
    return call;
  } else if (!type->isFloat()) {
    auto message = ErrorTextDecimalOf::takes(type->name(), type->isOptional());
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
    auto message = ErrorTextDecimalStatic::noMember(name);
    this->state.getDiagnostics().error(member->loc(), message);
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return call;
  }
  const auto *decimal = Type::decimalTy();
  const auto *array = Type::array(decimal);
  bool isSomeValue = false;
  bool isOk = true;
  for (size_t index = 0; index < call->args.size(); ++index) {
    Expr *&arg = call->args[index];
    if (!call->spreads(index)) {
      arg = this->argumentChecker->checkArg(arg, decimal);
      isSomeValue = true;
      continue;
    }
    arg = this->exprChecker->checkExpr(arg, array);
    if (arg->type != array && !arg->type->isError()) {
      auto message = ErrorTextDecimalStatic::spread(arg->type->name());
      this->state.getDiagnostics().error(arg->loc(), message);
      isOk = false;
    }
    isSomeValue |= this->indexProofs->canProveNotEmpty(arg);
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

Expr *DecimalChecker::checkRoundingMode(Expr *arg) {
  arg = this->argumentChecker->checkArg(arg, Type::string());
  if (arg->type->isError()) {
    return arg;
  }
  auto *literal = dyn_cast<StringLitExpr>(arg);
  if (literal != nullptr) {
    auto mode = this->roundingMode(literal->value);
    if (mode.isErr()) {
      this->state.getDiagnostics().error(arg->loc(), mode.error().message);
    }
    return arg;
  }
  auto modes = this->literalTypes->knownTexts(arg);
  if (!modes) {
    auto message = ErrorTextRoundingMode::notKnown();
    this->state.getDiagnostics().error(arg->loc(), message);
    return arg;
  }
  for (const std::string &mode : *modes) {
    std::u16string text(mode.begin(), mode.end());
    auto known = this->roundingMode(text);
    if (known.isErr()) {
      this->state.getDiagnostics().error(arg->loc(), known.error().message);
      break;
    }
  }

  return arg;
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
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
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
  llvm::StringRef use;
  for (const Replacement &r : kReplacements) {
    if (r.name == member->name) {
      use = r.use;
    }
  }
  auto message = ErrorTextDecimalMethod::noMember(member->name, use);
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
      bool isPositive = builtin == Builtin::DecDiv || builtin == Builtin::DecToExponential;
      auto places = ErrorTextDecimalCount::places();
      return this->checkDecimalCount(arg, places, isPositive ? 0 : -1000000000, 1000000000);
    }
    case DecimalArg::DIGITS: {
      auto digits = ErrorTextDecimalCount::digits();
      return this->checkDecimalCount(arg, digits, 1, 1000000000);
    }
    case DecimalArg::BASE: {
      auto base = ErrorTextDecimalCount::base();
      return this->checkDecimalCount(arg, base, 2, 36);
    }
    case DecimalArg::POWER: {
      auto power = ErrorTextDecimalCount::power();
      return this->checkDecimalCount(arg, power, -9007199254740991, 9007199254740991);
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
