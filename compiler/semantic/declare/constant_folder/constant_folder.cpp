#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/CheckedArithmetic.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

ConstantFolder::ConstantFolder(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.getScopes();
  this->typeDeclarer = &all.getTypeDeclarer();
}

int32_t ConstantFolder::jsToInt32(double x) {
  if (!std::isfinite(x)) {
    return 0;
  }
  double m = std::fmod(std::trunc(x), 4294967296.0);
  if (m < 0) {
    m += 4294967296.0;
  }

  return static_cast<int32_t>(static_cast<uint32_t>(m));
}

double ConstantFolder::power(double x, double y) {
  if (std::isnan(y) || (std::fabs(x) == 1 && std::isinf(y))) {
    return std::numeric_limits<double>::quiet_NaN();
  }

  return std::pow(x, y);
}

std::string ConstantFolder::formatNumber(double value) {
  char text[32];
  std::snprintf(text, sizeof text, "%.15g", value);

  return text;
}

int64_t ConstantFolder::wrapTo(const Type *type, int64_t value) {
  int bits = int(type->bitWidth());
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

bool ConstantFolder::fitsIn(const Type *type, int64_t value) {
  return value >= type->intMin() && (value < 0 || static_cast<uint64_t>(value) <= type->intMax());
}

bool ConstantFolder::exactEnough(const Type *type, int64_t value, bool wraps) {
  return ConstantFolder::fitsIn(type, value) &&
         (!wraps || !type->isInt() ||
          (value >= -ConstantFolder::maxExact && value <= ConstantFolder::maxExact));
}

std::pair<int64_t, std::optional<int64_t>> ConstantFolder::intPower(int64_t base,
                                                                    int64_t exponent) {
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

std::string ConstantFolder::wideText(int64_t a, int64_t b, BinaryOp op) {
  llvm::APInt x(128, static_cast<uint64_t>(a), true), y(128, static_cast<uint64_t>(b), true);
  llvm::APInt r = op == BinaryOp::Add ? x + y : op == BinaryOp::Sub ? x - y : x * y;

  llvm::SmallString<40> digits;
  r.toString(digits, 10, true);

  return std::string(digits);
}

std::string ConstantFolder::constantText(std::optional<int64_t> exact, double value) {
  if (exact) {
    return std::to_string(*exact);
  }
  if (std::fabs(value) >= 1e21) {
    return ConstantFolder::formatNumber(value);
  }
  char text[32];
  std::snprintf(text, sizeof text, "%.0f", value);

  return text;
}

std::optional<ConstantValue> ConstantFolder::evalConstant(const Expr *expr, EnumDecl *self,
                                                          size_t defined) {
  auto number = [](double value) {
    ConstantValue c;
    c.number = value;

    return c;
  };
  auto string = [](std::u16string text) {
    ConstantValue c;
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
      std::optional<ConstantValue> v = this->evalConstant(unary->operand, self, defined);
      if (!v || v->isString) {
        return std::nullopt;
      }
      switch (unary->op) {
        case UnaryOp::Neg:
          return number(-v->number);
        case UnaryOp::Plus:
          return v;
        case UnaryOp::BitNot:
          return number(~ConstantFolder::jsToInt32(v->number));
        default:
          return std::nullopt;
      }
    }
    case Expr::Kind::Binary: {
      auto *bin = cast<BinaryExpr>(expr);
      std::optional<ConstantValue> l = this->evalConstant(bin->lhs, self, defined);
      std::optional<ConstantValue> r = this->evalConstant(bin->rhs, self, defined);
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
          return number(ConstantFolder::power(a, b));
        case BinaryOp::BitAnd:
          return number(ConstantFolder::jsToInt32(a) & ConstantFolder::jsToInt32(b));
        case BinaryOp::BitOr:
          return number(ConstantFolder::jsToInt32(a) | ConstantFolder::jsToInt32(b));
        case BinaryOp::BitXor:
          return number(ConstantFolder::jsToInt32(a) ^ ConstantFolder::jsToInt32(b));
        case BinaryOp::Shl:
          return number(static_cast<int32_t>(static_cast<uint32_t>(ConstantFolder::jsToInt32(a))
                                             << (ConstantFolder::jsToInt32(b) & 31)));
        case BinaryOp::Shr:
          return number(ConstantFolder::jsToInt32(a) >> (ConstantFolder::jsToInt32(b) & 31));
        case BinaryOp::UShr:
          return number(static_cast<uint32_t>(ConstantFolder::jsToInt32(a)) >>
                        (ConstantFolder::jsToInt32(b) & 31));
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
      EnumDecl *decl = object ? this->scopes->lookup(object->name).enumDecl() : nullptr;
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
      this->typeDeclarer->resolveEnum(decl);
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
          !ConstantFolder::fitsIn(expr->type, static_cast<int64_t>(value))) {
        return std::nullopt;
      }
      return static_cast<int64_t>(value);
    }
    case Expr::Kind::Name: {
      return this->state.getConstantStore().findConstVar(cast<NameExpr>(expr)->var());
    }
    case Expr::Kind::Cast: {
      auto *conv = cast<CastExpr>(expr);
      if (!conv->isImplicit()) {
        return std::nullopt;
      }
      std::optional<int64_t> value = this->intConstant(conv->operand);
      if (!value) {
        return std::nullopt;
      }
      return ConstantFolder::fitsIn(conv->type, *value) ? std::optional<int64_t>(*value)
                                                        : std::nullopt;
    }
    default: {
      return this->state.getConstantStore().findIntConstant(expr);
    }
  }
}

std::optional<uint64_t> ConstantFolder::unsignedConstant(const Expr *expr) const {
  if (!expr->type || !expr->type->isUnsigned() || expr->type->bitWidth() != 64) {
    std::optional<int64_t> value = this->intConstant(expr);
    return value && *value >= 0 ? std::optional<uint64_t>(uint64_t(*value)) : std::nullopt;
  }
  auto *lit = dyn_cast<IntLitExpr>(expr);
  if (lit) {
    return lit->value;
  }
  auto *name = dyn_cast<NameExpr>(expr);
  ConstantStore &constants = this->state.getConstantStore();
  std::optional<uint64_t> bigVar = name ? constants.findBigUnsignedVar(name->var()) : std::nullopt;
  if (bigVar) {
    return bigVar;
  }
  if (std::optional<uint64_t> big = constants.findBigUnsigned(expr)) {
    return big;
  }
  std::optional<int64_t> value = this->intConstant(expr);

  return value && *value >= 0 ? std::optional<uint64_t>(uint64_t(*value)) : std::nullopt;
}

void ConstantFolder::noteUnsignedVar(const VarSymbol *var, const Expr *init) {
  if (!var->type || !var->type->isUnsigned() || var->type->bitWidth() != 64) {
    return;
  }
  std::optional<uint64_t> value = this->unsignedConstant(init);
  if (value && *value > uint64_t(std::numeric_limits<int64_t>::max())) {
    this->state.getConstantStore().rememberBigUnsignedVar(var, *value);
  }
}

void ConstantFolder::foldUnary(UnaryExpr *unary, bool wraps) {
  const Type *type = unary->type;
  if (type->isWrapping()) {
    return;
  }
  auto *lit = dyn_cast<IntLitExpr>(unary->operand);
  if (lit && unary->op == UnaryOp::Neg) {
    uint64_t magnitude = lit->value;
    if (magnitude <= uint64_t(1) << 63 &&
        ConstantFolder::fitsIn(type, static_cast<int64_t>(0 - magnitude))) {
      this->state.getConstantStore().rememberIntConstant(unary,
                                                         static_cast<int64_t>(0 - magnitude));
    }
    return;
  }
  std::optional<int64_t> value = this->intConstant(unary->operand);
  if (!value) {
    return;
  }
  switch (unary->op) {
    case UnaryOp::Plus:
      this->state.getConstantStore().rememberIntConstant(unary, *value);
      return;
    case UnaryOp::BitNot:
      this->state.getConstantStore().rememberIntConstant(
          unary, ConstantFolder::wrapTo(type, ~ConstantFolder::wrapTo(type, *value)));
      return;
    case UnaryOp::Neg: {
      std::optional<int64_t> exact = llvm::checkedSub(int64_t(0), *value);
      if (exact && ConstantFolder::exactEnough(type, *exact, wraps)) {
        this->state.getConstantStore().rememberIntConstant(unary, *exact);
        return;
      }
      std::string shown = exact ? ConstantFolder::constantText(exact, 0)
                                : ConstantFolder::wideText(0, *value, BinaryOp::Sub);
      int64_t negated = static_cast<int64_t>(0 - static_cast<uint64_t>(*value));
      int64_t wrapped = ConstantFolder::wrapTo(type, negated);
      std::string message = ConstantFolder::overflowText(unary->type, shown, wrapped, wraps);
      this->state.getDiagnostics().error(unary->loc(), message);
      return;
    }
    default:
      return;
  }
}

void ConstantFolder::foldBinary(BinaryExpr *bin, bool wraps) {
  const Type *type = bin->type;
  if (type->isWrapping() || bin->lhs->type->isWrapping()) {
    return;
  }
  if (type->isUnsigned() && type->bitWidth() == 64) {
    std::optional<uint64_t> l = this->unsignedConstant(bin->lhs),
                            r = this->unsignedConstant(bin->rhs);
    if (!l || !r) {
      return;
    }
    uint64_t ua = *l, ub = *r;
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
      llvm::APInt x(128, ua), y(128, ub);
      llvm::APInt wide = bin->op == BinaryOp::Add   ? x + y
                         : bin->op == BinaryOp::Sub ? x - y
                                                    : x * y;
      llvm::SmallString<48> digits;
      wide.toString(digits, 10, true);
      std::string shown(digits);
      std::string message = ConstantFolder::overflowText(type, shown, 0, wraps);
      this->state.getDiagnostics().error(bin->loc(), message);
    } else if (*result <= uint64_t(std::numeric_limits<int64_t>::max())) {
      this->state.getConstantStore().rememberIntConstant(bin, static_cast<int64_t>(*result));
    } else {
      this->state.getConstantStore().rememberBigUnsigned(bin, *result);
    }
    return;
  }
  std::optional<int64_t> l = this->intConstant(bin->lhs), r = this->intConstant(bin->rhs);
  if (!l || !r) {
    return;
  }
  int64_t a = *l, b = *r;
  auto ua = static_cast<uint64_t>(a), ub = static_cast<uint64_t>(b);
  std::optional<int64_t> exact;
  int64_t wrapped = 0;
  double value = 0;
  std::string text;
  switch (bin->op) {
    case BinaryOp::Add:
      exact = llvm::checkedAdd(a, b);
      wrapped = static_cast<int64_t>(ua + ub);
      text = ConstantFolder::wideText(a, b, bin->op);
      break;
    case BinaryOp::Sub:
      exact = llvm::checkedSub(a, b);
      wrapped = static_cast<int64_t>(ua - ub);
      text = ConstantFolder::wideText(a, b, bin->op);
      break;
    case BinaryOp::Mul:
      exact = llvm::checkedMul(a, b);
      wrapped = static_cast<int64_t>(ua * ub);
      text = ConstantFolder::wideText(a, b, bin->op);
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
        text = ConstantFolder::wideText(0, a, BinaryOp::Sub);
      } else {
        exact = wrapped = bin->op == BinaryOp::Div ? a / b : a % b;
      }
      break;
    }
    case BinaryOp::Pow: {
      if (b < 0) {
        return;
      }
      auto [low, product] = ConstantFolder::intPower(a, b);
      exact = product;
      wrapped = low;
      value = std::pow(static_cast<double>(a), static_cast<double>(b));
      break;
    }
    case BinaryOp::BitAnd:
      exact = ConstantFolder::wrapTo(type, a) & ConstantFolder::wrapTo(type, b);
      break;
    case BinaryOp::BitOr:
      exact = ConstantFolder::wrapTo(type, a) | ConstantFolder::wrapTo(type, b);
      break;
    case BinaryOp::BitXor:
      exact = ConstantFolder::wrapTo(type, a) ^ ConstantFolder::wrapTo(type, b);
      break;
    case BinaryOp::Shl:
    case BinaryOp::Shr:
    case BinaryOp::UShr: {
      const Type *shifted = bin->lhs->type;
      int width = int(shifted->bitWidth());
      ResultShiftCountFits counted = ConstantFolder::shiftCount(shifted, b);
      if (counted.isErr()) {
        this->state.getDiagnostics().error(bin->rhs->loc(), counted.error().message);
      }
      if (b < 0 || b >= int64_t(width)) {
        return;
      }
      int count = int(b);
      int64_t x = ConstantFolder::wrapTo(shifted, a);
      uint64_t pattern = width == 64 ? static_cast<uint64_t>(x)
                                     : static_cast<uint64_t>(x) & ((uint64_t(1) << width) - 1);
      if (bin->op == BinaryOp::Shl) {
        llvm::APInt start(128, uint64_t(a), !shifted->isUnsigned());
        llvm::APInt wide = start.shl(unsigned(count));
        int bitsUsed =
            shifted->isUnsigned() ? int(wide.getActiveBits()) : int(wide.getSignificantBits());
        bool fits = bitsUsed <= width;
        if (!fits && !shifted->isWrapping()) {
          llvm::SmallString<48> digits;
          wide.toString(digits, 10, true);
          std::string shown(digits);
          std::string message = ConstantFolder::overflowText(bin->type, shown, 0, false);
          this->state.getDiagnostics().error(bin->loc(), message);
          return;
        }
        exact = ConstantFolder::wrapTo(shifted, static_cast<int64_t>(pattern << count));
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
  if (exact && ConstantFolder::exactEnough(type, *exact, wraps)) {
    this->state.getConstantStore().rememberIntConstant(bin, *exact);
    return;
  }
  std::string shown = text.empty() ? ConstantFolder::constantText(exact, value) : text;
  int64_t wrappedValue = ConstantFolder::wrapTo(type, wrapped);
  std::string message = ConstantFolder::overflowText(bin->type, shown, wrappedValue, wraps);
  this->state.getDiagnostics().error(bin->loc(), message);
}

ConstantFolder::ResultDivisorNotZero ConstantFolder::checkDivisor(BinaryOp op,
                                                                  const Expr *divisor) const {
  bool divides = op == BinaryOp::Div || op == BinaryOp::Rem;
  if (divides && this->intConstant(divisor) == 0) {
    std::string message = ErrorTextDivisor::zero();

    return err(ConstantFolderCheckDivisorError{
        .code = ConstantFolderCheckDivisorErrorCode::DIVISION_BY_ZERO,
        .message = message,
    });
  }

  return ok(divisor);
}

ConstantFolder::ResultShiftCountFits ConstantFolder::shiftCount(const Type *shifted,
                                                                int64_t count) {
  int width = int(shifted->bitWidth());
  bool outside = count < 0 || count >= int64_t(width);
  if (outside && !shifted->isWrapping()) {
    std::string message = ErrorTextShiftCount::outside(count, width - 1, shifted->name());

    return err(ConstantFolderShiftCountError{
        .code = ConstantFolderShiftCountErrorCode::OUTSIDE_BITS,
        .message = message,
    });
  }

  return ok(shifted);
}

std::string ConstantFolder::overflowText(const Type *type, const std::string &value,
                                         int64_t wrapped, bool wraps) {
  if (wraps) {
    return ErrorTextOverflow::pastExact(value);
  }
  if (type->bitWidth() < 64) {
    return ErrorTextOverflow::inInt(value, type->name(), wrapped);
  }

  return ErrorTextOverflow::inDouble(value, type->name(), wrapped);
}

Expr *ConstantFolder::globalConstant(GlobalName object, llvm::StringRef name, llvm::SMLoc loc) {
  struct Entry {
    GlobalName object;
    llvm::StringLiteral name;
    double value;
  };
  static const Entry known[] = {
      {GlobalName::MATH, "E", 2.718281828459045},
      {GlobalName::MATH, "LN10", 2.302585092994046},
      {GlobalName::MATH, "LN2", 0.6931471805599453},
      {GlobalName::MATH, "LOG10E", 0.4342944819032518},
      {GlobalName::MATH, "LOG2E", 1.4426950408889634},
      {GlobalName::MATH, "PI", 3.141592653589793},
      {GlobalName::MATH, "SQRT1_2", 0.7071067811865476},
      {GlobalName::MATH, "SQRT2", 1.4142135623730951},
      {GlobalName::NUMBER, "MAX_SAFE_INTEGER", 9007199254740991.0},
      {GlobalName::NUMBER, "MIN_SAFE_INTEGER", -9007199254740991.0},
      {GlobalName::NUMBER, "EPSILON", 2.220446049250313e-16},
      {GlobalName::NUMBER, "MAX_VALUE", std::numeric_limits<double>::max()},
      {GlobalName::NUMBER, "MIN_VALUE", std::numeric_limits<double>::denorm_min()},
  };
  for (const Entry &entry : known) {
    if (entry.object == object && entry.name == name) {
      auto *lit = this->state.getAst().make<FloatLitExpr>(loc, entry.value);
      lit->type = Type::doubleTy();
      return lit;
    }
  }

  return nullptr;
}
}
