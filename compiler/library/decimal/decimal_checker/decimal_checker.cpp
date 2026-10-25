#include "compiler/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/binary_checker/binary_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/index_proofs/index_proofs.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/declare/constant_folder/constant_folder.h"
#include "llvm/Support/ConvertUTF.h"
#include <cstring>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

// The source text of the two sides of `bin`; empty if it is not known.
std::pair<llvm::StringRef, llvm::StringRef> sidesOf(const BinaryExpr *bin) {
  llvm::StringRef source = bin->source;
  const char *at = bin->loc().getPointer();
  size_t length = std::strlen(opSpelling(bin->op));
  if (source.empty() || at < source.begin() || at + length > source.end()) {
    return {};
  }

  return {llvm::StringRef(source.begin(), static_cast<size_t>(at - source.begin())).trim(),
          llvm::StringRef(at + length, static_cast<size_t>(source.end() - at - length)).trim()};
}

// Methods of BigNumber.js that a decimal does without: operators and other
// methods do what they do.
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

// A decimal power is exact with an integer exponent; a fraction of one would
// be a root.
std::string exponentMessage(const Type *found) {
  std::string message = "the exponent of a decimal power must be an integer ('int' or 'long'), "
                        "found '" +
                        found->name().str() + "'";
  if (found->isFloat() || found->isDecimal()) {
    message += "; for a square root, use 'x.sqrt()'";
  }

  return message;
}

std::string utf8(const std::u16string &text) {
  std::string result;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      result);

  return result;
}

} // namespace

DecimalChecker::DecimalChecker(Checker &checker) : checker_(checker) {}

bool DecimalChecker::isLiteralTree(const Expr *expr) {
  switch (expr->kind()) {
  case Expr::Kind::IntLit:
  case Expr::Kind::FloatLit:
    return true;
  case Expr::Kind::Unary: {
    auto *unary = cast<UnaryExpr>(expr);
    return (unary->op == UnaryOp::Neg || unary->op == UnaryOp::Plus) &&
           isLiteralTree(unary->operand);
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
      return isLiteralTree(bin->lhs) && isLiteralTree(bin->rhs);
    default:
      return false;
    }
  }
  default:
    return false;
  }
}

const Type *DecimalChecker::literalHint(const Expr *other, const Type *hint) {
  return other->type && other->type->nonOptional()->isDecimal() ? Type::decimalTy() : hint;
}

void DecimalChecker::checkComparisonOperands(BinaryExpr *bin) {
  // A string or boolean literal next to a value of a literal type or a union
  // takes its literal type: `s.kind == "circle"` (see sema_union.cpp).
  auto literal = [&](const Expr *e) {
    if (auto *unary = dyn_cast<UnaryExpr>(e); unary && unary->op == UnaryOp::Neg) {
      e = unary->operand;
    }

    return isLiteralTree(e) || isa<StringLitExpr>(e) || isa<BoolLitExpr>(e);
  };
  auto hint = [&](const Expr *other) {
    const Type *type = other->type ? other->type->nonOptional() : nullptr;
    if (type && (type->isLiteral() || type->isUnion())) {
      return other->type;
    }

    return literalHint(other, nullptr);
  };
  if (literal(bin->lhs) && !literal(bin->rhs)) {
    bin->rhs = checker_.exprChecker().checkExpr(bin->rhs);
    bin->lhs = checker_.exprChecker().checkExpr(bin->lhs, hint(bin->rhs));
    return;
  }
  bin->lhs = checker_.exprChecker().checkExpr(bin->lhs);
  bin->rhs =
      checker_.exprChecker().checkExpr(bin->rhs, literal(bin->rhs) ? hint(bin->lhs) : nullptr);
}

std::optional<DecimalLiteral> DecimalChecker::decimalLiteral(const Expr *expr) {
  if (auto *unary = dyn_cast<UnaryExpr>(expr); unary && unary->op == UnaryOp::Neg) {
    expr = unary->operand;
  }
  if (auto *lit = dyn_cast<IntLitExpr>(expr)) {
    return lit->text.empty() ? decimalOf(lit->value) : readDecimalLiteral(lit->text);
  }
  if (auto *lit = dyn_cast<FloatLitExpr>(expr)) {
    return readDecimalLiteral(lit->text);
  }

  return std::nullopt;
}

void DecimalChecker::checkDecimalLiteralRange(const Expr *lit) {
  std::optional<DecimalLiteral> value = decimalLiteral(lit);
  if (!value) {
    checker_.diags_.error(lit->loc(), "this number cannot be read as a decimal");
    return;
  }
  if (!value->isZero() && value->top() > kDecimalMaxExp) {
    checker_.diags_.error(lit->loc(), "this decimal is out of range: a decimal has at most " +
                                          std::to_string(kDecimalMaxExp + 1) +
                                          " digits before the point");
  }
}

void DecimalChecker::reportDecimalMix(const BinaryExpr *bin) {
  const Type *l = bin->lhs->type, *r = bin->rhs->type;
  bool leftDecimal = l->nonOptional()->isDecimal();
  const Type *other = leftDecimal ? r : l;
  if (!other->nonOptional()->isFloat()) {
    checker_.binaryChecker().reportOperandTypes(bin->loc(), opSpelling(bin->op), l, r);
    return;
  }
  std::string message = (llvm::Twine("operator '") + opSpelling(bin->op) +
                         "' cannot be applied to types '" + l->name() + "' and '" + r->name() +
                         "': a double is inexact already, and a decimal would take that on "
                         "silently; convert it explicitly")
                            .str();
  auto [lhs, rhs] = sidesOf(bin);
  if (other->isFloat() && !lhs.empty() && !rhs.empty()) {
    message += leftDecimal
                   ? (": '" + lhs + " " + opSpelling(bin->op) + " decimal(" + rhs + ")'").str()
                   : (": 'decimal(" + lhs + ") " + opSpelling(bin->op) + " " + rhs + "'").str();
  } else {
    message += " with 'decimal(x)'";
  }
  checker_.diags_.error(bin->loc(), message);
}

Expr *DecimalChecker::checkDecimalArithmetic(BinaryExpr *bin) {
  const Type *decimal = Type::decimalTy();
  const Type *l = bin->lhs->type, *r = bin->rhs->type;
  bin->type = Type::error();
  if (bin->op == BinaryOp::Pow) {
    if (!l->isDecimal()) {
      checker_.binaryChecker().reportOperandTypes(bin->loc(), "**", l, r);
      return bin;
    }
    if (!r->isInteger()) {
      checker_.diags_.error(bin->rhs->loc(), exponentMessage(r));
      return bin;
    }
    bin->rhs = checker_.typeRelations().convert(bin->rhs, Type::longTy());
    bin->type = decimal;
    return bin;
  }
  for (Expr **side : {&bin->lhs, &bin->rhs}) {
    const Type *type = (*side)->type;
    if (type->isDecimal()) {
      continue;
    }
    if (!type->isInteger()) {
      reportDecimalMix(bin);
      return bin;
    }
    *side = checker_.typeRelations().convert(*side, decimal);
  }
  checkDecimalDivisor(bin->op, bin->rhs);
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
    if (!type->isInteger()) {
      reportDecimalMix(bin);
      return bin;
    }
    *side = checker_.typeRelations().convert(*side, Type::decimalTy());
  }

  return bin;
}

void DecimalChecker::checkDecimalAssign(AssignExpr *assign, BinaryOp op) {
  const Type *valueType = assign->value->type;
  if (op == BinaryOp::Pow) {
    if (!valueType->isInteger()) {
      checker_.diags_.error(assign->value->loc(), exponentMessage(valueType));
      return;
    }
    assign->value = checker_.typeRelations().convert(assign->value, Type::longTy());
    return;
  }
  bool arithmetic = op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul ||
                    op == BinaryOp::Div || op == BinaryOp::Rem;
  if (!arithmetic) {
    checker_.binaryChecker().reportOperandTypes(
        assign->loc(), (std::string(opSpelling(op)) + "=").c_str(), assign->type, valueType);
    return;
  }
  assign->value = checker_.typeRelations().coerce(assign->value, Type::decimalTy());
  checkDecimalDivisor(op, assign->value);
}

void DecimalChecker::checkDecimalDivisor(BinaryOp op, const Expr *divisor) {
  if (op != BinaryOp::Div && op != BinaryOp::Rem) {
    return;
  }
  // A literal, or an integer constant made a decimal.
  const Expr *value = divisor;
  for (auto *conv = dyn_cast<CastExpr>(value); conv && conv->isImplicit();
       conv = dyn_cast<CastExpr>(value)) {
    value = conv->operand;
  }
  std::optional<DecimalLiteral> literal = decimalLiteral(value);
  bool zero = literal ? literal->isZero() : checker_.constantFolder().intConstant(value) == 0;
  if (zero) {
    checker_.diags_.error(divisor->loc(),
                          "decimal division by zero; a decimal has no Infinity or NaN");
  }
}

// decimal(x) of a string, a number or a decimal. A literal is read exactly;
// any other double is taken as the shortest digits that read back as it,
// which keeps the error of `0.1 + 0.2` (BigNumber.js does the same).
Expr *DecimalChecker::checkDecimalCall(CallExpr *call, llvm::SMLoc loc) {
  call->type = Type::error();
  if (!checker_.argumentChecker().checkArity(call, loc, 1, 1)) {
    return call;
  }
  Expr *&arg = call->args[0];
  bool literal = decimalLiteral(arg).has_value();
  arg = checker_.exprChecker().checkExpr(arg, literal ? Type::decimalTy() : nullptr);
  const Type *type = arg->type;
  if (type->isError()) {
    return call;
  }
  if (type->isInteger() || type->isDecimal()) {
    arg = checker_.typeRelations().convert(arg, Type::decimalTy());
  } else if (!type->isString() && !type->isFloat()) {
    std::string message =
        "decimal() takes a string, a number or a decimal, found '" + type->name().str() + "'";
    if (type->isOptional()) {
      message += "; the value may be undefined: check it first or give a default with '??'";
    }
    checker_.diags_.error(arg->loc(), message);
    return call;
  }
  call->builtin = Builtin::DecimalOf;
  call->type = Type::decimalTy();

  return call;
}

// decimal.max, decimal.min and decimal.sum: of values and of arrays spread
// into the call, `decimal.sum(...prices)`.
Expr *DecimalChecker::checkDecimalStatic(CallExpr *call, MemberExpr *member) {
  llvm::StringRef name = member->name;
  call->type = Type::error();
  Builtin builtin = name == "max"   ? Builtin::DecMax
                    : name == "min" ? Builtin::DecMin
                    : name == "sum" ? Builtin::DecSum
                                    : Builtin::None;
  if (builtin == Builtin::None) {
    std::string message = "property '" + name.str() + "' does not exist on 'decimal'";
    if (name == "maximum" || name == "minimum") {
      message += "; use 'decimal." + name.substr(0, 3).str() + "'";
    } else if (name == "config" || name == "set") {
      message += ": there are no settings for all decimals; division takes its places and "
                 "rounding as arguments, 'a.div(b, places, mode)'";
    }
    checker_.diags_.error(member->loc(), message);
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
    return call;
  }
  const Type *decimal = Type::decimalTy(), *array = Type::array(decimal);
  bool someValue = false, ok = true;
  for (size_t i = 0; i < call->args.size(); ++i) {
    Expr *&arg = call->args[i];
    if (!call->spreads(i)) {
      arg = checker_.argumentChecker().checkArg(arg, decimal);
      someValue = true;
      continue;
    }
    arg = checker_.exprChecker().checkExpr(arg, array);
    if (arg->type != array && !arg->type->isError()) {
      checker_.diags_.error(arg->loc(), "only an array of decimals can be spread here, found '" +
                                            arg->type->name() + "'");
      ok = false;
    }
    someValue |= checker_.indexProofs().provesNotEmpty(arg);
  }
  if (!ok) {
    return call;
  }
  if (!someValue && builtin != Builtin::DecSum) {
    if (call->args.empty()) {
      checker_.diags_.error(member->loc(), "decimal." + name + "() needs at least one value");
    } else {
      checker_.diags_.error(call->loc(),
                            "decimal." + name +
                                " of an array that may be empty has no value; "
                                "check that the array is not empty, or pass a first value: "
                                "'decimal." +
                                name + "(0, ...a)'");
    }
    return call;
  }
  call->builtin = builtin;
  call->type = decimal;

  return call;
}

Expr *DecimalChecker::checkRoundingMode(Expr *arg) {
  arg = checker_.argumentChecker().checkArg(arg, Type::string());
  if (auto *lit = dyn_cast<StringLitExpr>(arg); lit && decimalRoundingMode(lit->value) < 0) {
    checker_.diags_.error(arg->loc(), "unknown rounding mode '" + utf8(lit->value) +
                                          "'; the modes are " + kRoundingModes);
  }

  return arg;
}

Expr *DecimalChecker::checkDecimalCount(Expr *arg, const char *what) {
  arg = checker_.exprChecker().checkExpr(arg, Type::longTy());
  const Type *type = arg->type;
  if (type->isError()) {
    return arg;
  }
  if (!type->isInteger()) {
    std::string message = std::string(what) + " must be an integer ('int' or 'long'), found '" +
                          type->name().str() + "'";
    if (type->isOptional()) {
      message += "; the value may be undefined: check it first or give a default with '??'";
    }
    checker_.diags_.error(arg->loc(), message);
    return arg;
  }

  return checker_.typeRelations().convert(arg, Type::longTy());
}

// The methods of a decimal: those of BigNumber.js that operators do not
// replace, with rounding modes as strings (BigNumber.ROUND_HALF_UP is
// "half-up"). `round` is decimalPlaces(dp, mode) and integerValue(mode).
Expr *DecimalChecker::checkDecimalMethod(CallExpr *call, MemberExpr *member) {
  struct Method {
    llvm::StringLiteral name;
    Builtin builtin;
    unsigned minArgs, maxArgs;
    // The kinds of the arguments: 'd' a decimal, 'p' decimal places, 's'
    // significant digits, 'b' a base, 'k' a power of ten, 'm' a rounding
    // mode.
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
      arg = checker_.exprChecker().checkExpr(arg);
    }
  };
  if (!method) {
    std::string message = "property '" + name.str() + "' does not exist on type 'decimal'";
    for (const Replacement &r : kReplacements) {
      if (r.name == name) {
        message += "; a decimal uses " + r.use.str();
      }
    }
    if (name == "isNaN" || name == "isFinite") {
      message += "; a decimal is never NaN or infinite";
    }
    checker_.diags_.error(member->loc(), message);
    checkArgsLoosely();
    return call;
  }
  if (method->builtin == Builtin::DecDecimalPlaces && !call->args.empty()) {
    checker_.diags_.error(member->loc(), "decimalPlaces() only counts the places; to round, use "
                                         "'x.round(places, mode)'");
    checkArgsLoosely();
    return call;
  }
  if (!checker_.argumentChecker().checkArity(call, member->loc(), method->minArgs,
                                             method->maxArgs)) {
    return call;
  }
  for (size_t i = 0; i < call->args.size(); ++i) {
    Expr *&arg = call->args[i];
    switch (method->args[i]) {
    case 'd':
      arg = checker_.argumentChecker().checkArg(arg, decimal);
      break;
    case 'p':
      arg = checkDecimalCount(arg, "the number of decimal places");
      break;
    case 's':
      arg = checkDecimalCount(arg, "the number of significant digits");
      break;
    case 'b':
      arg = checkDecimalCount(arg, "the base");
      break;
    case 'k':
      arg = checkDecimalCount(arg, "the power of ten");
      break;
    default:
      arg = checkRoundingMode(arg);
      break;
    }
  }
  if (method->builtin == Builtin::DecDiv) {
    checkDecimalDivisor(BinaryOp::Div, call->args[0]);
  }
  call->builtin = method->builtin;
  call->type = method->ret;

  return call;
}

void DecimalChecker::reportBigInt(llvm::SMLoc loc, llvm::StringRef what) {
  checker_.diags_.error(
      loc, what + " is not supported, as there is no bigint: integers up to 9.2e18 fit in "
                  "'long', longer ones in 'decimal' (e.g. 'decimal(2) ** 100')");
}

} // namespace tz
