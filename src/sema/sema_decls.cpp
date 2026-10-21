// Type aliases, enums and the constants of the global objects.

#include "sema/sema.h"

#include "llvm/Support/ConvertUTF.h"

#include <cmath>
#include <cstdio>
#include <limits>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

std::u16string toUtf16(llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);
  return std::u16string(units.begin(), units.end());
}

// JavaScript's ToInt32.
int32_t jsToInt32(double x) {
  if (!std::isfinite(x))
    return 0;
  double m = std::fmod(std::trunc(x), 4294967296.0);
  if (m < 0)
    m += 4294967296.0;
  return static_cast<int32_t>(static_cast<uint32_t>(m));
}

// JavaScript's `**`: unlike C's pow, 1 ** NaN and (-1) ** Infinity are NaN.
double power(double x, double y) {
  if (std::isnan(y) || (std::fabs(x) == 1 && std::isinf(y)))
    return std::numeric_limits<double>::quiet_NaN();
  return std::pow(x, y);
}

std::string formatNumber(double value) {
  char text[32];
  std::snprintf(text, sizeof text, "%.15g", value);
  return text;
}

} // namespace

//===----------------------------------------------------------------------===//
// Declarations
//===----------------------------------------------------------------------===//

void Sema::declareTypes(llvm::ArrayRef<Stmt *> stmts) {
  std::vector<Stmt *> declared;
  for (Stmt *stmt : stmts) {
    llvm::StringRef name;
    if (auto *alias = dyn_cast<TypeAliasDecl>(stmt)) {
      name = alias->name;
      if (Type::fromName(name) || name == "Array") {
        // `type int = number` tells tsc what an int is; here it is built in.
        bool integer = name == "int" || name == "long" || name == "double";
        bool forTsc = integer && alias->type.kind == TypeRef::Kind::Name && alias->type.name == "number";
        if (!forTsc)
          diags_.error(alias->loc(), "'" + name + "' is a built-in type and cannot be redefined" +
                                         (integer ? "; only 'type " + name.str() +
                                                        " = number', for tsc, may declare it"
                                                  : std::string()));
        continue;
      }
    } else if (auto *decl = dyn_cast<EnumDecl>(stmt)) {
      name = decl->name;
      declare(name, decl->loc(), {nullptr, nullptr, decl});
    } else {
      continue;
    }
    auto [it, inserted] = typeScopes_.back().try_emplace(name, stmt);
    if (!inserted) {
      diags_.error(stmt->loc(), "duplicate declaration of type '" + name + "'");
      diags_.note(it->second->loc(), "previously declared here");
      continue;
    }
    declared.push_back(stmt);
  }
  // Resolved here, in the scope that declares them, so that the names they
  // use mean what they mean here.
  for (Stmt *stmt : declared) {
    if (auto *alias = dyn_cast<TypeAliasDecl>(stmt))
      resolveAlias(alias);
    else
      resolveEnum(cast<EnumDecl>(stmt));
  }
}

Stmt *Sema::lookupType(llvm::StringRef name) const {
  for (auto scope = typeScopes_.rbegin(); scope != typeScopes_.rend(); ++scope) {
    auto found = scope->find(name);
    if (found != scope->end())
      return found->second;
  }
  return nullptr;
}

const Type *Sema::resolveAlias(TypeAliasDecl *alias) {
  if (alias->resolved)
    return alias->resolved;
  if (alias->resolving) {
    diags_.error(alias->loc(), "type alias '" + alias->name + "' refers to itself");
    alias->resolved = Type::error();
    return alias->resolved;
  }
  alias->resolving = true;
  const Type *type = resolveTypeInner(alias->type);
  alias->resolving = false;
  if (!alias->resolved)
    alias->resolved = type;
  return alias->resolved;
}

//===----------------------------------------------------------------------===//
// Enums
//===----------------------------------------------------------------------===//

void Sema::resolveEnum(EnumDecl *decl) {
  if (decl->resolved || decl->resolving) {
    if (decl->resolving)
      diags_.error(decl->loc(), "the members of enum '" + decl->name + "' depend on themselves");
    return;
  }
  decl->resolving = true;
  double next = 0;
  bool counting = true; // the next member without a value gets `next`
  bool numbers = false, strings = false;
  for (size_t i = 0; i < decl->members.size(); ++i) {
    EnumMember &m = decl->members[i];
    for (size_t j = 0; j < i; ++j)
      if (decl->members[j].name == m.name)
        diags_.error(m.loc, "duplicate enum member '" + m.name + "'");
    double value = next;
    if (m.init) {
      std::optional<Constant> c = evalConstant(m.init, decl, i);
      if (!c) {
        diags_.error(m.init->loc(), "the value of an enum member must be a constant: a number, a "
                                    "string, or an expression of them and of other members");
        counting = false;
        continue;
      }
      if (c->isString) {
        m.isString = true;
        m.text = c->text;
        strings = true;
        counting = false;
        continue;
      }
      value = c->number;
    } else if (!counting) {
      diags_.error(m.loc, "enum member '" + m.name +
                              "' needs a value, since the member before it is not a number");
      continue;
    }
    if (value != std::trunc(value) || value < std::numeric_limits<int32_t>::min() ||
        value > std::numeric_limits<int32_t>::max()) {
      diags_.error(m.init ? m.init->loc() : m.loc, "the value of enum member '" + m.name +
                                                       "' must be an integer that fits in "
                                                       "'int', found " + formatNumber(value));
      counting = false;
      continue;
    }
    m.number = static_cast<int32_t>(value);
    numbers = true;
    next = value + 1;
    counting = true;
  }
  if (numbers && strings)
    diags_.error(decl->loc(), "enum '" + decl->name + "' cannot mix numbers and strings");
  decl->isString = strings && !numbers;
  decl->resolving = false;
  decl->resolved = true;
}

std::optional<Sema::Constant> Sema::evalConstant(const Expr *expr, EnumDecl *self, size_t defined) {
  auto number = [](double value) {
    Constant c;
    c.number = value;
    return c;
  };
  auto string = [](std::u16string text) {
    Constant c;
    c.isString = true;
    c.text = std::move(text);
    return c;
  };
  auto member = [&](const EnumMember &m) { return m.isString ? string(m.text) : number(m.number); };

  switch (expr->kind()) {
  case Expr::Kind::IntLit:
    return number(static_cast<double>(cast<IntLitExpr>(expr)->value));
  case Expr::Kind::FloatLit:
    return number(cast<FloatLitExpr>(expr)->value);
  case Expr::Kind::StringLit:
    return string(cast<StringLitExpr>(expr)->value);
  case Expr::Kind::Template: {
    auto *tmpl = cast<TemplateExpr>(expr);
    if (!tmpl->exprs.empty())
      return std::nullopt;
    return string(tmpl->quasis[0]);
  }
  case Expr::Kind::Unary: {
    auto *unary = cast<UnaryExpr>(expr);
    std::optional<Constant> v = evalConstant(unary->operand, self, defined);
    if (!v || v->isString)
      return std::nullopt;
    switch (unary->op) {
    case UnaryOp::Neg: return number(-v->number);
    case UnaryOp::Plus: return v;
    case UnaryOp::BitNot: return number(~jsToInt32(v->number));
    default: return std::nullopt;
    }
  }
  case Expr::Kind::Binary: {
    auto *bin = cast<BinaryExpr>(expr);
    std::optional<Constant> l = evalConstant(bin->lhs, self, defined);
    std::optional<Constant> r = evalConstant(bin->rhs, self, defined);
    if (!l || !r)
      return std::nullopt;
    if (l->isString || r->isString) {
      if (bin->op != BinaryOp::Add || !l->isString || !r->isString)
        return std::nullopt;
      return string(l->text + r->text);
    }
    double a = l->number, b = r->number;
    switch (bin->op) {
    case BinaryOp::Add: return number(a + b);
    case BinaryOp::Sub: return number(a - b);
    case BinaryOp::Mul: return number(a * b);
    case BinaryOp::Div: return number(a / b);
    case BinaryOp::Rem: return number(std::fmod(a, b));
    case BinaryOp::Pow: return number(power(a, b));
    case BinaryOp::BitAnd: return number(jsToInt32(a) & jsToInt32(b));
    case BinaryOp::BitOr: return number(jsToInt32(a) | jsToInt32(b));
    case BinaryOp::BitXor: return number(jsToInt32(a) ^ jsToInt32(b));
    case BinaryOp::Shl:
      return number(static_cast<int32_t>(static_cast<uint32_t>(jsToInt32(a)) << (jsToInt32(b) & 31)));
    case BinaryOp::Shr: return number(jsToInt32(a) >> (jsToInt32(b) & 31));
    case BinaryOp::UShr:
      return number(static_cast<uint32_t>(jsToInt32(a)) >> (jsToInt32(b) & 31));
    default: return std::nullopt;
    }
  }
  case Expr::Kind::Name: {
    // An earlier member of the same enum.
    llvm::StringRef name = cast<NameExpr>(expr)->name;
    for (size_t i = 0; i < defined; ++i)
      if (self->members[i].name == name)
        return member(self->members[i]);
    return std::nullopt;
  }
  case Expr::Kind::Member: {
    // `E.X`, of this enum or another one.
    auto *access = cast<MemberExpr>(expr);
    auto *object = dyn_cast<NameExpr>(access->object);
    EnumDecl *decl = object ? lookup(object->name).enumDecl : nullptr;
    if (!decl)
      return std::nullopt;
    if (decl == self) {
      for (size_t i = 0; i < defined; ++i)
        if (self->members[i].name == access->name)
          return member(self->members[i]);
      return std::nullopt;
    }
    resolveEnum(decl);
    if (const EnumMember *m = decl->find(access->name))
      return member(*m);
    return std::nullopt;
  }
  default:
    return std::nullopt;
  }
}

// `E.X` is the member's value, as a literal.
Expr *Sema::checkEnumMember(EnumDecl *decl, MemberExpr *member, const Type *expected) {
  resolveEnum(decl);
  const EnumMember *m = decl->find(member->name);
  if (!m) {
    diags_.error(member->loc(), "property '" + member->name + "' does not exist on enum '" +
                                    decl->name + "'");
    member->type = Type::error();
    return member;
  }
  if (m->isString) {
    auto *lit = ctx_.make<StringLitExpr>(member->loc(), m->text);
    lit->type = Type::string();
    return lit;
  }
  int64_t value = m->number;
  Expr *lit = ctx_.make<IntLitExpr>(member->loc(), static_cast<uint64_t>(value < 0 ? -value : value));
  if (value < 0)
    lit = ctx_.make<UnaryExpr>(member->loc(), UnaryOp::Neg, lit);
  return checkExpr(lit, expected);
}

// `E[value]`, the name of the member with that value, as in TypeScript. For
// a value no member has, JavaScript gives undefined.
Expr *Sema::checkEnumIndex(EnumDecl *decl, IndexExpr *index) {
  resolveEnum(decl);
  index->type = Type::error();
  index->index = checkExpr(index->index, Type::longTy());
  const Type *type = index->index->type;
  if (type->isError())
    return index;
  if (decl->isString) {
    diags_.error(index->loc(), "enum '" + decl->name +
                                   "' has string values, and only numbers map back to names");
    return index;
  }
  if (!type->isInteger()) {
    diags_.error(index->index->loc(), "the value of an enum member is an integer, found '" +
                                          type->name() + "'");
    return index;
  }
  index->index = convert(index->index, Type::longTy());
  // A constant: the name is known now. When several members have the value,
  // the last one names it.
  if (std::optional<Linear> value = linear(index->index); value && value->term.isZero()) {
    const EnumMember *found = nullptr;
    for (const EnumMember &m : decl->members)
      if (m.number == value->offset)
        found = &m;
    if (!found) {
      auto *undefined = ctx_.make<UndefinedExpr>(index->loc());
      undefined->type = Type::undefinedTy();
      return undefined;
    }
    auto *lit = ctx_.make<StringLitExpr>(index->loc(), toUtf16(found->name));
    lit->type = Type::string();
    return lit;
  }
  index->enumDecl = decl;
  index->type = Type::optional(Type::string());
  return index;
}

//===----------------------------------------------------------------------===//
// Constants of the global objects
//===----------------------------------------------------------------------===//

Expr *Sema::globalConstant(llvm::StringRef object, llvm::StringRef name, llvm::SMLoc loc) {
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
      auto *lit = ctx_.make<FloatLitExpr>(loc, entry.value);
      lit->type = Type::doubleTy();
      return lit;
    }
  }
  return nullptr;
}

} // namespace tz
