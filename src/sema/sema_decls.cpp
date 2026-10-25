// Type aliases, enums and the constants of the global objects.

#include "sema/sema.h"

#include "llvm/Support/CheckedArithmetic.h"
#include "llvm/Support/ConvertUTF.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <string>

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

std::vector<Stmt *> Sema::declareTypes(llvm::ArrayRef<Stmt *> stmts, bool resolve) {
  std::vector<Stmt *> declared;
  for (Stmt *stmt : stmts) {
    llvm::StringRef name;
    if (auto *alias = dyn_cast<TypeAliasDecl>(stmt)) {
      name = alias->name;
      if (Type::fromName(name) || name == "Array") {
        // `type int = number` tells tsc what an int is; here it is built in.
        bool integer = name == "int" || name == "long" || name == "double";
        bool forTsc = integer && alias->type.kind == TypeRef::Kind::Name && alias->type.name == "number";
        // So is the union tsc needs for Json (docs/modules.md, section 5).
        forTsc |= name == "Json" && alias->type.kind == TypeRef::Kind::Union;
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
    } else if (auto *cls = dyn_cast<ClassDecl>(stmt); cls && scopes_.size() == 1) {
      // Only at the top level of a file; elsewhere checkClassStmt reports it.
      name = cls->name;
      declareClass(cls);
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
  if (resolve)
    resolveTypes(declared);
  return declared;
}

// Resolved in the scope that declares them, so that the names they use mean
// what they mean there. A generic alias names a type for each list of type
// arguments; here it is checked once with its type parameters.
void Sema::resolveTypes(llvm::ArrayRef<Stmt *> declared) {
  // An alias of `typeof x` waits for x, until a type uses it.
  std::function<bool(const TypeRef &)> typeOf = [&](const TypeRef &ref) {
    return ref.kind == TypeRef::Kind::TypeOf || llvm::any_of(ref.args, typeOf);
  };
  for (Stmt *stmt : declared) {
    if (auto *alias = dyn_cast<TypeAliasDecl>(stmt); alias && typeOf(alias->type))
      continue;
    if (auto *alias = dyn_cast<TypeAliasDecl>(stmt); alias && !alias->typeParams.empty()) {
      instantiateAlias(alias, aliasTypeParams(alias), alias->loc());
    } else if (alias) {
      resolveAlias(alias);
    } else if (auto *cls = dyn_cast<ClassDecl>(stmt)) {
      // A class whose base is a value is resolved where it is declared.
      if (!cls->baseExpr)
        resolveClass(cls);
    } else {
      resolveEnum(cast<EnumDecl>(stmt));
    }
  }
}

Stmt *Sema::lookupType(llvm::StringRef name) const {
  for (auto scope = typeScopes_.rbegin(); scope != typeScopes_.rend(); ++scope) {
    auto found = scope->find(name);
    if (found != scope->end())
      return found->second;
  }
  // The utilities of TypeScript that are mapped types (see declarePrelude).
  if (TypeAliasDecl *alias = builtinAliases_.lookup(name))
    return alias;
  // Error and its kinds (see Sema::lookup).
  return errorKindClass(name);
}

// An object type may refer to itself through its fields (`next?: Node`,
// `children: Tree[]`): those get a placeholder for it, which becomes the type
// once its fields are known. Such a type stays tied to its declaration; any
// other object type is the same as every other one with the same fields.
const Type *Sema::resolveAlias(TypeAliasDecl *alias) {
  if (alias->resolved)
    return alias->resolved;
  // A type of another file, imported before it is resolved: in its own file.
  if (Module *home = declModules_.lookup(alias); home && home != module_ && scopes_.size() == 1) {
    Module *here = module_;
    useModule(home);
    const Type *type = resolveAlias(alias);
    useModule(here);
    return type;
  }
  if (alias->resolving) {
    if (alias->self) {
      alias->recursive = true;
      return alias->self;
    }
    diags_.error(alias->loc(), "type alias '" + alias->name + "' refers to itself");
    alias->resolved = Type::error();
    return alias->resolved;
  }
  alias->resolving = true;
  const Type *type;
  if (alias->type.kind == TypeRef::Kind::Object) {
    alias->self = Type::declareObject(alias->name);
    // `interface User extends Named`: the fields of Named come first.
    std::vector<const Type *> bases;
    std::vector<Overload> overloads;
    std::vector<Field> fields = alias->bases.empty() ? resolveFields(alias->type, &overloads)
                                                     : interfaceFields(alias, bases, overloads);
    if (alias->recursive) {
      alias->self->defineFields(std::move(fields), std::move(overloads));
      type = alias->self;
    } else {
      type = Type::object(std::move(fields), alias->name, std::move(overloads));
    }
    for (const Type *base : bases)
      if (!type->isError())
        addBase(type, base);
  } else if (alias->type.kind == TypeRef::Kind::Union) {
    // A union whose members refer back to it in their fields, as the nodes
    // of a tree do: the fields get it as declared, with its members after.
    alias->self = Type::declareUnion(alias->name);
    type = resolveTypeInner(alias->type);
    if (alias->recursive && !type->isError()) {
      if (type->isUnion()) {
        alias->self->defineMembers(type);
        type = alias->self;
      } else {
        diags_.error(alias->loc(), "type alias '" + alias->name +
                                       "' refers to itself, and only a union of several types "
                                       "without undefined can: make a field optional instead, "
                                       "e.g. 'next?: " + alias->name + "'");
        type = Type::error();
      }
    }
  } else {
    type = resolveTypeInner(alias->type);
  }
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
  if (Module *home = declModules_.lookup(decl); home && home != module_ && scopes_.size() == 1) {
    Module *here = module_;
    useModule(home);
    resolveEnum(decl);
    useModule(here);
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
// Integer constants
//===----------------------------------------------------------------------===//

namespace {

// Doubles hold every integer up to 2^53 exactly.
constexpr int64_t kMaxExact = int64_t(1) << 53;

// `value` wrapped around to `bits` bits, as the machine computes it.
int64_t wrapTo(unsigned bits, int64_t value) {
  return bits == 32 ? static_cast<int32_t>(static_cast<uint32_t>(value)) : value;
}

bool fitsIn(unsigned bits, int64_t value) { return wrapTo(bits, value) == value; }

// base ** exponent wrapped around to 64 bits, and the true value if it fits.
std::pair<int64_t, std::optional<int64_t>> intPower(int64_t base, int64_t exponent) {
  uint64_t wrapped = 1, factor = static_cast<uint64_t>(base);
  std::optional<int64_t> exact = 1, exactFactor = base;
  for (; exponent > 0; exponent >>= 1) {
    if (exponent & 1) {
      wrapped *= factor;
      exact = exact && exactFactor ? llvm::checkedMul(*exact, *exactFactor) : std::nullopt;
    }
    factor *= factor;
    if (exponent > 1)
      exactFactor = exactFactor ? llvm::checkedMul(*exactFactor, *exactFactor) : std::nullopt;
  }
  return {static_cast<int64_t>(wrapped), exact};
}

// The true value of an integer constant as JavaScript prints it (up to 1e21).
std::string constantText(std::optional<int64_t> exact, double value) {
  if (exact)
    return std::to_string(*exact);
  if (std::fabs(value) >= 1e21)
    return formatNumber(value);
  char text[32];
  std::snprintf(text, sizeof text, "%.0f", value);
  return text;
}

} // namespace

std::optional<int64_t> Sema::intConstant(const Expr *expr) const {
  if (!expr->type || !expr->type->isInteger())
    return std::nullopt;
  switch (expr->kind()) {
  case Expr::Kind::IntLit: {
    // One that does not fit was reported; `-2147483648` is a UnaryExpr.
    uint64_t value = cast<IntLitExpr>(expr)->value;
    if (value > uint64_t(std::numeric_limits<int64_t>::max()) ||
        !fitsIn(expr->type->bitWidth(), static_cast<int64_t>(value)))
      return std::nullopt;
    return static_cast<int64_t>(value);
  }
  case Expr::Kind::Name: {
    auto found = constVars_.find(cast<NameExpr>(expr)->var);
    if (found == constVars_.end())
      return std::nullopt;
    return found->second;
  }
  case Expr::Kind::Cast: {
    // An int widened to long: the value the int holds. A long cut to an int
    // for a bitwise operator: its low 32 bits.
    auto *conv = cast<CastExpr>(expr);
    if (!conv->isImplicit())
      return std::nullopt;
    std::optional<int64_t> value = intConstant(conv->operand);
    if (!value)
      return std::nullopt;
    return wrapTo(std::min(conv->operand->type->bitWidth(), conv->type->bitWidth()), *value);
  }
  default: {
    auto found = constants_.find(expr);
    if (found == constants_.end())
      return std::nullopt;
    return found->second;
  }
  }
}

void Sema::foldUnary(UnaryExpr *unary, bool wraps) {
  unsigned bits = unary->type->bitWidth();
  if (auto *lit = dyn_cast<IntLitExpr>(unary->operand); lit && unary->op == UnaryOp::Neg) {
    // `-2147483648` fits in an int, though 2147483648 alone does not.
    uint64_t magnitude = lit->value;
    if (magnitude <= uint64_t(1) << (bits - 1))
      constants_[unary] = static_cast<int64_t>(0 - magnitude);
    return;
  }
  std::optional<int64_t> value = intConstant(unary->operand);
  if (!value)
    return;
  switch (unary->op) {
  case UnaryOp::Plus:
    constants_[unary] = *value;
    return;
  case UnaryOp::BitNot:
    constants_[unary] = ~wrapTo(bits, *value);
    return;
  case UnaryOp::Neg: {
    std::optional<int64_t> exact = llvm::checkedSub(int64_t(0), *value);
    if (exact && (fitsIn(bits, *exact) || (wraps && *exact >= -kMaxExact && *exact <= kMaxExact))) {
      constants_[unary] = *exact;
      return;
    }
    reportOverflow(unary, unary->type, constantText(exact, -static_cast<double>(*value)),
                   wrapTo(bits, static_cast<int64_t>(0 - static_cast<uint64_t>(*value))), wraps);
    return;
  }
  default:
    return;
  }
}

// In a `wraps` context only the low bits of the value matter, as in
// `(h * 31 + c) | 0`: JavaScript computes the true value and `| 0` takes its
// low 32 bits, which is what wrapping around gives. So there the value may
// exceed its type, but not 2^53, beyond which JavaScript rounds it.
void Sema::foldBinary(BinaryExpr *bin, bool wraps) {
  std::optional<int64_t> l = intConstant(bin->lhs), r = intConstant(bin->rhs);
  if (!l || !r)
    return;
  unsigned bits = bin->type->bitWidth();
  int64_t a = *l, b = *r;
  auto ua = static_cast<uint64_t>(a), ub = static_cast<uint64_t>(b);
  std::optional<int64_t> exact; // the true value, if it fits in 64 bits
  int64_t wrapped = 0;          // what the program computes, before wrapTo
  double value = 0;             // what JavaScript computes
  switch (bin->op) {
  case BinaryOp::Add:
    exact = llvm::checkedAdd(a, b);
    wrapped = static_cast<int64_t>(ua + ub);
    value = static_cast<double>(a) + static_cast<double>(b);
    break;
  case BinaryOp::Sub:
    exact = llvm::checkedSub(a, b);
    wrapped = static_cast<int64_t>(ua - ub);
    value = static_cast<double>(a) - static_cast<double>(b);
    break;
  case BinaryOp::Mul:
    exact = llvm::checkedMul(a, b);
    wrapped = static_cast<int64_t>(ua * ub);
    value = static_cast<double>(a) * static_cast<double>(b);
    break;
  case BinaryOp::Div:
  case BinaryOp::Rem: {
    if (b == 0) // reported by checkDivisor
      return;
    int64_t min = bits == 32 ? std::numeric_limits<int32_t>::min() : std::numeric_limits<int64_t>::min();
    if (b == -1) { // MIN / -1 wraps around to MIN (see CodeGen::emitIntDivRem)
      exact = bin->op == BinaryOp::Rem ? 0 : llvm::checkedSub(int64_t(0), a);
      wrapped = bin->op == BinaryOp::Rem ? 0 : (a == min ? min : -a);
      value = -static_cast<double>(a);
    } else {
      exact = wrapped = bin->op == BinaryOp::Div ? a / b : a % b; // truncated, as integers divide
    }
    break;
  }
  case BinaryOp::Pow: {
    if (b < 0) // reported by checkBinary
      return;
    auto [low, product] = intPower(a, b);
    exact = product;
    wrapped = low;
    value = std::pow(static_cast<double>(a), static_cast<double>(b));
    break;
  }
  case BinaryOp::BitAnd:
    exact = wrapTo(bits, a) & wrapTo(bits, b);
    break;
  case BinaryOp::BitOr:
    exact = wrapTo(bits, a) | wrapTo(bits, b);
    break;
  case BinaryOp::BitXor:
    exact = wrapTo(bits, a) ^ wrapTo(bits, b);
    break;
  case BinaryOp::Shl:
  case BinaryOp::Shr:
  case BinaryOp::UShr: {
    // As CodeGen does it: the count masked to the width, `>>>` on the bits.
    // An int's `>>>` may give a long (see checkBinary), which holds the
    // unsigned 32-bit result.
    unsigned width = bin->lhs->type->bitWidth();
    unsigned count = static_cast<unsigned>(b) & (width - 1);
    int64_t x = wrapTo(width, a);
    uint64_t pattern = width == 32 ? static_cast<uint32_t>(x) : static_cast<uint64_t>(x);
    if (bin->op == BinaryOp::Shl)
      exact = wrapTo(bits, static_cast<int64_t>(pattern << count));
    else if (bin->op == BinaryOp::Shr)
      exact = x >> count;
    else
      exact = wrapTo(bits, static_cast<int64_t>(pattern >> count));
    break;
  }
  default:
    return;
  }
  if (exact && (fitsIn(bits, *exact) || (wraps && *exact >= -kMaxExact && *exact <= kMaxExact))) {
    constants_[bin] = *exact;
    return;
  }
  reportOverflow(bin, bin->type, constantText(exact, value), wrapTo(bits, wrapped), wraps);
}

void Sema::checkDivisor(BinaryOp op, const Expr *divisor) {
  if ((op == BinaryOp::Div || op == BinaryOp::Rem) && intConstant(divisor) == 0)
    diags_.error(divisor->loc(), "integer division by zero; to get Infinity or NaN as in "
                                 "JavaScript, divide doubles, e.g. '1.0 / 0'");
}

void Sema::reportOverflow(const Expr *expr, const Type *type, const std::string &value,
                          int64_t wrapped, bool wraps) {
  if (wraps) {
    diags_.error(expr->loc(), "this constant is " + value +
                                  ", beyond 2^53, where JavaScript rounds it before taking its "
                                  "low bits; for a 32-bit product use Math.imul");
    return;
  }
  bool isInt = type->bitWidth() == 32;
  diags_.error(expr->loc(), "this constant is " + value + ", which does not fit in '" +
                                type->name() + "' and would wrap around to " +
                                std::to_string(wrapped) + "; compute it as a " +
                                (isInt ? "'long' or a 'double', e.g. 'const x: long = ...'"
                                       : "'double', e.g. 'const x: double = ...'"));
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
