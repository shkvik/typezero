#include "compiler/declare/constant_folder/constant_folder.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/declare/type_declarer/type_declarer.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/CheckedArithmetic.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

int32_t jsToInt32(double x) {
  if (!std::isfinite(x)) {
    return 0;
  }
  double m = std::fmod(std::trunc(x), 4294967296.0);
  if (m < 0) {
    m += 4294967296.0;
  }

  return static_cast<int32_t>(static_cast<uint32_t>(m));
}

double power(double x, double y) {
  if (std::isnan(y) || (std::fabs(x) == 1 && std::isinf(y))) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  return std::pow(x, y);
}

std::string formatNumber(double value) {
  char text[32];
  std::snprintf(text, sizeof text, "%.15g", value);

  return text;
}

constexpr int64_t kMaxExact = int64_t(1) << 53;

int64_t wrapTo(const Type *type, int64_t value) {
  unsigned bits = type->bitWidth();
  if (bits == 64) {
    return value;
  }
  uint64_t low = static_cast<uint64_t>(value) & ((uint64_t(1) << bits) - 1);
  if (type->isUnsigned()) {
    return static_cast<int64_t>(low);
  }
  uint64_t sign = uint64_t(1) << (bits - 1);

  return static_cast<int64_t>((low ^ sign) - sign);
}

bool fitsIn(const Type *type, int64_t value) {
  return value >= type->intMin() && (value < 0 || static_cast<uint64_t>(value) <= type->intMax());
}

const Type *bitType(const Type *type) { return type->isInt() ? Type::integer(32, false) : type; }

bool exactEnough(const Type *type, int64_t value, bool wraps) {
  return fitsIn(type, value) &&
         (!wraps || !type->isInt() || (value >= -kMaxExact && value <= kMaxExact));
}

std::pair<int64_t, std::optional<int64_t>> intPower(int64_t base, int64_t exponent) {
  uint64_t wrapped = 1, factor = static_cast<uint64_t>(base);
  std::optional<int64_t> exact = 1, exactFactor = base;
  for (; exponent > 0; exponent >>= 1) {
    if (exponent & 1) {
      wrapped *= factor;
      exact = exact && exactFactor ? llvm::checkedMul(*exact, *exactFactor) : std::nullopt;
    }
    factor *= factor;
    if (exponent > 1) {
      exactFactor = exactFactor ? llvm::checkedMul(*exactFactor, *exactFactor) : std::nullopt;
    }
  }

  return {static_cast<int64_t>(wrapped), exact};
}

std::string wideText(int64_t a, int64_t b, BinaryOp op) {
  llvm::APInt x(128, static_cast<uint64_t>(a), true), y(128, static_cast<uint64_t>(b), true);
  llvm::APInt r = op == BinaryOp::Add ? x + y : op == BinaryOp::Sub ? x - y : x * y;

  llvm::SmallString<40> digits;
  r.toString(digits, 10, true);

  return std::string(digits);
}

std::string constantText(std::optional<int64_t> exact, double value) {
  if (exact) {
    return std::to_string(*exact);
  }
  if (std::fabs(value) >= 1e21) {
    return formatNumber(value);
  }
  char text[32];
  std::snprintf(text, sizeof text, "%.0f", value);

  return text;
}

}

ConstantFolder::ConstantFolder(Checker &checker) : checker_(checker) {}

std::optional<Checker::Constant> ConstantFolder::evalConstant(const Expr *expr, EnumDecl *self,
                                                              size_t defined) {
  auto number = [](double value) {
    Checker::Constant c;
    c.number = value;

    return c;
  };
  auto string = [](std::u16string text) {
    Checker::Constant c;
    c.isString = true;
    c.text = std::move(text);

    return c;
  };
  auto member = [&](const EnumMember &m) {
    return m.isString ? string(m.text) : number(static_cast<double>(m.number));
  };

  switch (expr->kind()) {
  case Expr::Kind::IntLit:
    return number(static_cast<double>(cast<IntLitExpr>(expr)->value));
  case Expr::Kind::FloatLit:
    return number(cast<FloatLitExpr>(expr)->value);
  case Expr::Kind::StringLit:
    return string(cast<StringLitExpr>(expr)->value);
  case Expr::Kind::Template: {
    auto *tmpl = cast<TemplateExpr>(expr);
    if (!tmpl->exprs.empty()) {
      return std::nullopt;
    }
    return string(tmpl->quasis[0]);
  }
  case Expr::Kind::Unary: {
    auto *unary = cast<UnaryExpr>(expr);
    std::optional<Checker::Constant> v = evalConstant(unary->operand, self, defined);
    if (!v || v->isString) {
      return std::nullopt;
    }
    switch (unary->op) {
    case UnaryOp::Neg:
      return number(-v->number);
    case UnaryOp::Plus:
      return v;
    case UnaryOp::BitNot:
      return number(~jsToInt32(v->number));
    default:
      return std::nullopt;
    }
  }
  case Expr::Kind::Binary: {
    auto *bin = cast<BinaryExpr>(expr);
    std::optional<Checker::Constant> l = evalConstant(bin->lhs, self, defined);
    std::optional<Checker::Constant> r = evalConstant(bin->rhs, self, defined);
    if (!l || !r) {
      return std::nullopt;
    }
    if (l->isString || r->isString) {
      if (bin->op != BinaryOp::Add || !l->isString || !r->isString) {
        return std::nullopt;
      }
      return string(l->text + r->text);
    }
    double a = l->number, b = r->number;
    switch (bin->op) {
    case BinaryOp::Add:
      return number(a + b);
    case BinaryOp::Sub:
      return number(a - b);
    case BinaryOp::Mul:
      return number(a * b);
    case BinaryOp::Div:
      return number(a / b);
    case BinaryOp::Rem:
      return number(std::fmod(a, b));
    case BinaryOp::Pow:
      return number(power(a, b));
    case BinaryOp::BitAnd:
      return number(jsToInt32(a) & jsToInt32(b));
    case BinaryOp::BitOr:
      return number(jsToInt32(a) | jsToInt32(b));
    case BinaryOp::BitXor:
      return number(jsToInt32(a) ^ jsToInt32(b));
    case BinaryOp::Shl:
      return number(
          static_cast<int32_t>(static_cast<uint32_t>(jsToInt32(a)) << (jsToInt32(b) & 31)));
    case BinaryOp::Shr:
      return number(jsToInt32(a) >> (jsToInt32(b) & 31));
    case BinaryOp::UShr:
      return number(static_cast<uint32_t>(jsToInt32(a)) >> (jsToInt32(b) & 31));
    default:
      return std::nullopt;
    }
  }
  case Expr::Kind::Name: {
    llvm::StringRef name = cast<NameExpr>(expr)->name;
    for (size_t i = 0; i < defined; ++i) {
      if (self->members[i].name == name) {
        return member(self->members[i]);
      }
    }
    return std::nullopt;
  }
  case Expr::Kind::Member: {
    auto *access = cast<MemberExpr>(expr);
    auto *object = dyn_cast<NameExpr>(access->object);
    EnumDecl *decl = object ? checker_.scopes().lookup(object->name).enumDecl : nullptr;
    if (!decl) {
      return std::nullopt;
    }
    if (decl == self) {
      for (size_t i = 0; i < defined; ++i) {
        if (self->members[i].name == access->name) {
          return member(self->members[i]);
        }
      }
      return std::nullopt;
    }
    checker_.typeDeclarer().resolveEnum(decl);
    if (const EnumMember *m = decl->find(access->name)) {
      return member(*m);
    }
    return std::nullopt;
  }
  default:
    return std::nullopt;
  }
}

std::optional<int64_t> ConstantFolder::intConstant(const Expr *expr) const {
  if (!expr->type || !expr->type->isInteger()) {
    return std::nullopt;
  }
  switch (expr->kind()) {
  case Expr::Kind::IntLit: {
    uint64_t value = cast<IntLitExpr>(expr)->value;
    if (value > uint64_t(std::numeric_limits<int64_t>::max()) ||
        !fitsIn(expr->type, static_cast<int64_t>(value))) {
      return std::nullopt;
    }
    return static_cast<int64_t>(value);
  }
  case Expr::Kind::Name: {
    auto found = checker_.constVars_.find(cast<NameExpr>(expr)->var);
    if (found == checker_.constVars_.end()) {
      return std::nullopt;
    }
    return found->second;
  }
  case Expr::Kind::Cast: {
    auto *conv = cast<CastExpr>(expr);
    if (!conv->isImplicit()) {
      return std::nullopt;
    }
    std::optional<int64_t> value = intConstant(conv->operand);
    if (!value) {
      return std::nullopt;
    }
    return fitsIn(conv->type, *value) ? std::optional<int64_t>(*value) : std::nullopt;
  }
  default: {
    auto found = constants_.find(expr);
    if (found == constants_.end()) {
      return std::nullopt;
    }
    return found->second;
  }
  }
}

void ConstantFolder::foldUnary(UnaryExpr *unary, bool wraps) {
  const Type *type = unary->type;
  if (auto *lit = dyn_cast<IntLitExpr>(unary->operand); lit && unary->op == UnaryOp::Neg) {
    uint64_t magnitude = lit->value;
    if (magnitude <= uint64_t(1) << 63 && fitsIn(type, static_cast<int64_t>(0 - magnitude))) {
      constants_[unary] = static_cast<int64_t>(0 - magnitude);
    }
    return;
  }
  std::optional<int64_t> value = intConstant(unary->operand);
  if (!value) {
    return;
  }
  switch (unary->op) {
  case UnaryOp::Plus:
    constants_[unary] = *value;
    return;
  case UnaryOp::BitNot:
    constants_[unary] = wrapTo(bitType(type), ~wrapTo(bitType(type), *value));
    return;
  case UnaryOp::Neg: {
    std::optional<int64_t> exact = llvm::checkedSub(int64_t(0), *value);
    if (exact && exactEnough(type, *exact, wraps)) {
      constants_[unary] = *exact;
      return;
    }
    reportOverflow(unary, unary->type,
                   exact ? constantText(exact, 0) : wideText(0, *value, BinaryOp::Sub),
                   wrapTo(type, static_cast<int64_t>(0 - static_cast<uint64_t>(*value))), wraps);
    return;
  }
  default:
    return;
  }
}

void ConstantFolder::foldBinary(BinaryExpr *bin, bool wraps) {
  std::optional<int64_t> l = intConstant(bin->lhs), r = intConstant(bin->rhs);
  if (!l || !r) {
    return;
  }
  const Type *type = bin->type;
  int64_t a = *l, b = *r;
  auto ua = static_cast<uint64_t>(a), ub = static_cast<uint64_t>(b);
  if (type->isUnsigned() && type->bitWidth() == 64) {
    std::optional<uint64_t> result;
    switch (bin->op) {
    case BinaryOp::Add:
      result = llvm::checkedAddUnsigned(ua, ub);
      break;
    case BinaryOp::Sub:
      result = ua >= ub ? std::optional<uint64_t>(ua - ub) : std::nullopt;
      break;
    case BinaryOp::Mul:
      result = llvm::checkedMulUnsigned(ua, ub);
      break;
    default:
      return;
    }
    if (!result) {
      reportOverflow(bin, type, wideText(a, b, bin->op), 0, wraps);
    } else if (*result <= uint64_t(std::numeric_limits<int64_t>::max())) {
      constants_[bin] = static_cast<int64_t>(*result);
    }
    return;
  }
  std::optional<int64_t> exact;
  int64_t wrapped = 0;
  double value = 0;
  std::string text;
  switch (bin->op) {
  case BinaryOp::Add:
    exact = llvm::checkedAdd(a, b);
    wrapped = static_cast<int64_t>(ua + ub);
    text = wideText(a, b, bin->op);
    break;
  case BinaryOp::Sub:
    exact = llvm::checkedSub(a, b);
    wrapped = static_cast<int64_t>(ua - ub);
    text = wideText(a, b, bin->op);
    break;
  case BinaryOp::Mul:
    exact = llvm::checkedMul(a, b);
    wrapped = static_cast<int64_t>(ua * ub);
    text = wideText(a, b, bin->op);
    break;
  case BinaryOp::Div:
  case BinaryOp::Rem: {
    if (b == 0) {
      return;
    }
    int64_t min = type->intMin();
    if (b == -1 && !type->isUnsigned()) {
      exact = bin->op == BinaryOp::Rem ? 0 : llvm::checkedSub(int64_t(0), a);
      wrapped = bin->op == BinaryOp::Rem ? 0 : (a == min ? min : -a);
      text = wideText(0, a, BinaryOp::Sub);
    } else {
      exact = wrapped = bin->op == BinaryOp::Div ? a / b : a % b;
    }
    break;
  }
  case BinaryOp::Pow: {
    if (b < 0) {
      return;
    }
    auto [low, product] = intPower(a, b);
    exact = product;
    wrapped = low;
    value = std::pow(static_cast<double>(a), static_cast<double>(b));
    break;
  }
  case BinaryOp::BitAnd:
    exact = wrapTo(bitType(type), a) & wrapTo(bitType(type), b);
    break;
  case BinaryOp::BitOr:
    exact = wrapTo(bitType(type), a) | wrapTo(bitType(type), b);
    break;
  case BinaryOp::BitXor:
    exact = wrapTo(bitType(type), a) ^ wrapTo(bitType(type), b);
    break;
  case BinaryOp::Shl:
  case BinaryOp::Shr:
  case BinaryOp::UShr: {
    const Type *shifted = bitType(bin->lhs->type);
    unsigned width = shifted->bitWidth();
    unsigned count = static_cast<unsigned>(b) & (width - 1);
    int64_t x = wrapTo(shifted, a);
    uint64_t pattern = width == 64 ? static_cast<uint64_t>(x)
                                   : static_cast<uint64_t>(x) & ((uint64_t(1) << width) - 1);
    if (bin->op == BinaryOp::Shl) {
      exact = wrapTo(shifted, static_cast<int64_t>(pattern << count));
    } else if (bin->op == BinaryOp::Shr && !shifted->isUnsigned()) {
      exact = x >> count;
    } else if ((pattern >> count) > uint64_t(std::numeric_limits<int64_t>::max())) {
      return;
    } else {
      exact = static_cast<int64_t>(pattern >> count);
    }
    break;
  }
  default:
    return;
  }
  if (exact && exactEnough(type, *exact, wraps)) {
    constants_[bin] = *exact;
    return;
  }
  reportOverflow(bin, bin->type, text.empty() ? constantText(exact, value) : text,
                 wrapTo(type, wrapped), wraps);
}

void ConstantFolder::checkDivisor(BinaryOp op, const Expr *divisor) {
  if ((op == BinaryOp::Div || op == BinaryOp::Rem) && intConstant(divisor) == 0) {
    checker_.diags_.error(divisor->loc(), "integer division by zero; to get Infinity or NaN as in "
                                          "JavaScript, divide doubles, e.g. '1.0 / 0'");
  }
}

void ConstantFolder::reportOverflow(const Expr *expr, const Type *type, const std::string &value,
                                    int64_t wrapped, bool wraps) {
  if (wraps) {
    checker_.diags_.error(expr->loc(),
                          "this constant is " + value +
                              ", beyond 2^53, where JavaScript rounds it before taking its "
                              "low bits; for a 32-bit product use Math.imul");
    return;
  }
  checker_.diags_.error(expr->loc(),
                        "this constant is " + value + ", which does not fit in '" + type->name() +
                            "' and would wrap around to " + std::to_string(wrapped) +
                            "; compute it as " +
                            (type->bitWidth() < 64 ? "an 'int', e.g. 'const x: int = ...'"
                                                   : "a 'double', e.g. 'const x: double = ...'"));
}

Expr *ConstantFolder::globalConstant(llvm::StringRef object, llvm::StringRef name,
                                     llvm::SMLoc loc) {
  constexpr double kInfinity = std::numeric_limits<double>::infinity();
  struct Entry {
    llvm::StringLiteral object, name;
    double value;
  };
  static const Entry constants[] = {
      {"", "NaN", std::numeric_limits<double>::quiet_NaN()},
      {"", "Infinity", kInfinity},
      {"Math", "E", 2.718281828459045},
      {"Math", "LN10", 2.302585092994046},
      {"Math", "LN2", 0.6931471805599453},
      {"Math", "LOG10E", 0.4342944819032518},
      {"Math", "LOG2E", 1.4426950408889634},
      {"Math", "PI", 3.141592653589793},
      {"Math", "SQRT1_2", 0.7071067811865476},
      {"Math", "SQRT2", 1.4142135623730951},
      {"Number", "MAX_SAFE_INTEGER", 9007199254740991.0},
      {"Number", "MIN_SAFE_INTEGER", -9007199254740991.0},
      {"Number", "EPSILON", 2.220446049250313e-16},
      {"Number", "MAX_VALUE", std::numeric_limits<double>::max()},
      {"Number", "MIN_VALUE", std::numeric_limits<double>::denorm_min()},
      {"Number", "POSITIVE_INFINITY", kInfinity},
      {"Number", "NEGATIVE_INFINITY", -kInfinity},
      {"Number", "NaN", std::numeric_limits<double>::quiet_NaN()},
  };
  for (const Entry &entry : constants) {
    if (entry.object == object && entry.name == name) {
      auto *lit = checker_.ctx_.make<FloatLitExpr>(loc, entry.value);
      lit->type = Type::doubleTy();
      return lit;
    }
  }

  return nullptr;
}

}
