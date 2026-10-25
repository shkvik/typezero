// Unions and literal types (docs/objects.md, section 7).
//
// A union `A | B` holds a value of any of its members. A literal type holds
// one value: `"circle"`, `0`, `true`. A literal gets its literal type where
// one is expected (`const d: Dir = "up"`, `s.kind == "circle"`), and its base
// type anywhere else, so `const d = "up"` is a string as before.
//
// Checks narrow a variable of a union to some of its members: the tag of a
// union of objects (`s.kind == "circle"`), `x == "up"`, `typeof`, `in`,
// `instanceof` and `Array.isArray`; a `switch` too, which must handle every
// value of a literal type it switches on. A field that every member has is
// read through the union: `s.kind`, `s.name`.

#include "compiler/check/sema/sema.h"

#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

std::string toUtf8(const std::u16string &text) {
  std::string utf8;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(text.data()), text.size()),
      utf8);
  return utf8;
}

// The members of a type: those of a union, or the type itself (so `type`
// must outlive the result).
llvm::ArrayRef<const Type *> membersOf(const Type *&type) {
  return type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
}

// Whether `type` is a literal type or a union of them.
bool isLiteralSet(const Type *type) {
  if (type->isLiteral())
    return true;
  return type->isUnion() &&
         llvm::all_of(type->members(), [](const Type *m) { return m->isLiteral(); });
}

// The part of `expected` in `slot` if it is made of literal types only, as
// `0 | 1` is: a literal written there takes its literal type.
const Type *literalPart(const Type *expected, UnionSlot slot) {
  if (!expected)
    return nullptr;
  const Type *part = expected->nonOptional()->slotPart(slot);
  return part && isLiteralSet(part) ? part : nullptr;
}

// The variable `expr` reads, through the conversions Sema adds where it is
// narrowed.
const VarSymbol *readVariable(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit() || conv->shortCircuit)
      return nullptr;
    expr = conv->operand;
  }
  auto *name = dyn_cast<NameExpr>(expr);
  return name ? name->var : nullptr;
}

// The literal type of a literal as written (before it is checked or after),
// looking through the conversions of a checked one.
const Type *writtenLiteral(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit())
      return nullptr;
    expr = conv->operand;
  }
  bool negated = false;
  if (auto *unary = dyn_cast<UnaryExpr>(expr); unary && unary->op == UnaryOp::Neg) {
    negated = true;
    expr = unary->operand;
  }
  if (auto *lit = dyn_cast<StringLitExpr>(expr))
    return negated ? nullptr : Type::stringLiteral(lit->value);
  if (auto *lit = dyn_cast<BoolLitExpr>(expr))
    return negated ? nullptr : Type::boolLiteral(lit->value);
  if (auto *lit = dyn_cast<IntLitExpr>(expr)) {
    double value = negated ? -double(lit->value) : double(lit->value);
    bool fits = value >= -2147483648.0 && value <= 2147483647.0 && !(negated && lit->value == 0);
    return Type::numberLiteral(value, fits ? Type::intTy() : Type::doubleTy());
  }
  if (auto *lit = dyn_cast<FloatLitExpr>(expr))
    return Type::numberLiteral(negated ? -lit->value : lit->value, Type::doubleTy());
  return nullptr;
}

} // namespace

bool hasParamMember(const Type *type) {
  return type->isUnion() &&
         llvm::any_of(type->members(), [](const Type *m) { return m->isParam(); });
}

const Type *Sema::unionMemberFor(const Type *from, const Type *to) {
  for (const Type *m : to->members())
    if (m == from)
      return m;
  // A literal goes into its base: "circle" into string.
  if (from->isLiteral())
    for (const Type *m : to->members())
      if (m == from->literalBase())
        return m;
  for (const Type *m : to->members())
    if (!m->isParam() && !from->isParam() && isImplicitlyConvertible(from, m))
      return m;
  return nullptr;
}

const Type *Sema::unionUnify(const Type *a, const Type *b) {
  if (const Type *common = unify(a, b))
    return common;
  if (a->isError() || b->isError())
    return Type::error();
  if (a->isVoid() || b->isVoid())
    return nullptr;
  // A decimal and a double do not mix, even in a union (docs/decimal.md).
  auto number = [](const Type *t) {
    t = t->nonOptional();
    return t->isNumeric() || t->isDecimal();
  };
  if (number(a) && number(b))
    return nullptr;
  const Type *joined = Type::unionOf({a, b});
  if (joined->isError() || !unionProblem(joined).empty())
    return nullptr;
  return joined;
}

const Type *Sema::literalExpectation(const Type *expected, unsigned depth) {
  if (!expected || depth > 8 || expected->nonOptional()->typeOp() != TypeOp::Conditional)
    return expected;
  // A conditional type that waits: what its branches expect.
  std::vector<const Type *> branches;
  for (const Type *branch : conditionalBranches(expected->nonOptional()))
    branches.push_back(literalExpectation(branch, depth + 1));
  return branches.empty() ? expected : Type::unionOf(branches);
}

const Type *Sema::stringLiteralFor(const Type *expected, const std::u16string &text) {
  expected = literalExpectation(expected);
  if (literalPart(expected, UnionSlot::Atom))
    return Type::stringLiteral(text);
  // A literal written where a string of a pattern is expected keeps its text,
  // which tells whether it is one.
  const Type *strings = expected ? expected->nonOptional()->slotPart(UnionSlot::String) : nullptr;
  if (strings &&
      llvm::any_of(strings->isUnion() ? strings->members() : llvm::ArrayRef<const Type *>(strings),
                   [](const Type *m) { return m->isPattern(); }))
    return Type::stringLiteral(text);
  return nullptr;
}

const Type *Sema::boolLiteralFor(const Type *expected, bool value) {
  expected = literalExpectation(expected);
  return literalPart(expected, UnionSlot::Bool) ? Type::boolLiteral(value) : nullptr;
}

const Type *Sema::numberLiteralFor(const Type *expected, double value, bool fraction) {
  expected = literalExpectation(expected);
  if (!fraction)
    if (const Type *part = literalPart(expected, UnionSlot::Int))
      if (llvm::is_contained(membersOf(part), Type::numberLiteral(value, Type::intTy())) ||
          !literalPart(expected, UnionSlot::Double))
        return Type::numberLiteral(value, Type::intTy());
  if (literalPart(expected, UnionSlot::Double))
    return Type::numberLiteral(value, Type::doubleTy());
  return nullptr;
}

const Type *Sema::numberMemberOf(const Type *expected, bool fraction) {
  if (!expected)
    return nullptr;
  const Type *type = expected->nonOptional();
  if (!type->isUnion())
    return nullptr;
  for (const Type *t : {Type::intTy(), Type::longTy(), Type::doubleTy(), Type::decimalTy()})
    if ((!fraction || !t->isInteger()) && llvm::is_contained(type->members(), t))
      return t;
  return nullptr;
}

const Type *Sema::literalMember(const ObjectLitExpr *lit, const Type *target) const {
  if (llvm::any_of(lit->props, [](const ObjectLitExpr::Property &p) { return p.spread; }))
    return nullptr;
  const Type *best = nullptr;
  int bestScore = -1;
  bool ambiguous = false;
  for (const Type *m : target->members()) {
    if (!m->isObject() || m->isClass() || m->isTuple() || m->isRegExp() || m->isMatch() ||
        m->isAnyObject() || m->isIncomplete())
      continue;
    // Exactly the fields of the type.
    bool fits = llvm::all_of(lit->props, [&](const ObjectLitExpr::Property &p) {
      return m->field(p.name) || m->overload(p.name);
    });
    for (const Field &f : m->fields())
      fits &= f.type->isOptional() ||
              llvm::any_of(lit->props,
                           [&](const ObjectLitExpr::Property &p) { return p.name == f.name; });
    if (!fits)
      continue;
    // The tags it gives tell the members apart: `kind: "circle"`.
    int score = 0;
    bool mismatch = false;
    for (const ObjectLitExpr::Property &p : lit->props) {
      const Field *f = m->field(p.name);
      const Type *tag = f && p.value ? writtenLiteral(p.value) : nullptr;
      if (!tag)
        continue;
      const Type *field = f->type->nonOptional();
      if (isImplicitlyConvertible(tag, field) ||
          (tag->literalBase()->isInt() && isImplicitlyConvertible(tag->literalBase(), field)))
        score +=
            isLiteralSet(field) || (field->isUnion() && field->slotPart(UnionSlot::Atom)) ? 1 : 0;
      else if (isLiteralSet(field) || field->isUnion())
        mismatch = true;
    }
    if (mismatch)
      continue;
    if (score > bestScore) {
      best = m;
      bestScore = score;
      ambiguous = false;
    } else if (score == bestScore) {
      ambiguous = true;
    }
  }
  return ambiguous ? nullptr : best;
}

const Type *Sema::typeIn(const Flow &flow, const VarSymbol *var) const {
  for (const auto &[narrowed, type] : flow.narrowed)
    if (narrowed == var)
      return type;
  if (var->type->isOptional() && flow.defined.contains(var))
    return var->type->inner();
  return var->type;
}

void Sema::narrowVariable(Flow &flow, const VarSymbol *var, const Type *type) {
  llvm::erase_if(flow.narrowed, [&](const auto &narrowed) { return narrowed.first == var; });
  if (!type) { // no member is left: nothing reaches here
    flow.narrowed.push_back({var, Type::never()});
    return;
  }
  if (type == var->type)
    return;
  if (var->type->isOptional() && type == var->type->inner()) {
    flow.defined.insert(var);
    return;
  }
  flow.narrowed.push_back({var, type});
  if (!type->isOptional())
    flow.defined.insert(var);
}

// `type` with only the members `keep` accepts; null if none is left.
static const Type *keepMembers(const Type *type, llvm::function_ref<bool(const Type *)> keep) {
  bool undefined = type->isOptional();
  const Type *value = type->nonOptional();
  std::vector<const Type *> kept;
  for (const Type *m : membersOf(value))
    if (keep(m))
      kept.push_back(m);
  bool keepsUndefined = undefined && keep(Type::undefinedTy());
  if (kept.empty())
    return keepsUndefined ? type : nullptr; // only undefined: nothing to learn
  const Type *result = Type::unionOf(kept);
  return keepsUndefined ? Type::optional(result) : result;
}

std::optional<Sema::NarrowTarget> Sema::narrowTarget(const Expr *expr) const {
  const Type *declared = nullptr;
  NarrowTarget target;
  if (const VarSymbol *var = readVariable(expr); var && var->type) {
    target.var = var;
    declared = var->type;
  } else {
    // A field read through fields from a variable: `o.shape`.
    const Expr *read = expr;
    while (auto *conv = dyn_cast<CastExpr>(read)) {
      if (!conv->isImplicit() || conv->shortCircuit)
        return std::nullopt;
      read = conv->operand;
    }
    auto *member = dyn_cast<MemberExpr>(read);
    std::optional<FieldPath> path = member ? fieldPath(member) : std::nullopt;
    if (!path)
      return std::nullopt;
    target.path = std::move(*path);
    declared = member->type;
  }
  const Type *base = declared->nonOptional();
  if (!base->isUnion() && !base->isLiteral() && !base->isBool())
    return std::nullopt;
  target.declared = declared;
  return target;
}

const Type *Sema::targetType(const Flow &flow, const NarrowTarget &target) const {
  if (target.var)
    return typeIn(flow, target.var);
  for (const auto &[path, type] : flow.narrowedFields)
    if (path == *target.path)
      return type;
  if (target.declared->isOptional() && llvm::is_contained(flow.definedFields, *target.path))
    return target.declared->inner();
  return target.declared;
}

void Sema::narrowTargetTo(Flow &flow, const NarrowTarget &target, const Type *type) const {
  if (target.var) {
    narrowVariable(flow, target.var, type);
    return;
  }
  const FieldPath &path = *target.path;
  llvm::erase_if(flow.narrowedFields, [&](const auto &entry) { return entry.first == path; });
  if (type && !type->isOptional())
    defineField(flow, path);
  if (type == target.declared ||
      (type && target.declared->isOptional() && type == target.declared->inner()))
    return;
  flow.narrowedFields.push_back({path, type ? type : Type::never()});
}

void Sema::addUnionFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const {
  auto narrow = [&](Flow &flow, const NarrowTarget &target,
                    llvm::function_ref<bool(const Type *)> keep) {
    const Type *now = targetType(flow, target);
    if (now->isNever())
      return;
    narrowTargetTo(flow, target, keepMembers(now, keep));
  };

  // `Array.isArray(x)`
  if (auto *call = dyn_cast<CallExpr>(cond); call && call->builtin == Builtin::ArrayIsArray) {
    if (std::optional<NarrowTarget> target = narrowTarget(call->args[0])) {
      narrow(whenTrue, *target, [](const Type *m) { return m->isArray(); });
      narrow(whenFalse, *target, [](const Type *m) { return !m->isArray(); });
    }
    return;
  }
  // The tag of a union of objects, `s.kind` (in `s.kind == "circle"` and in
  // `if (r.ok)`): the members whose tag `keep` accepts.
  // With `s?.kind`, undefined gives undefined, which `undefinedKept` says
  // whether to keep.
  auto narrowByTag = [&](const MemberExpr *member, Flow &flow,
                         llvm::function_ref<bool(const Type *)> keep, bool undefinedKept = true) {
    const Expr *object = member->object;
    if (member->optional) {
      auto *link = dyn_cast<CastExpr>(object);
      if (!link || !link->shortCircuit)
        return false;
      object = link->operand;
    }
    std::optional<NarrowTarget> target = narrowTarget(object);
    if (!target || !target->declared->nonOptional()->isUnion())
      return false;
    // A union, or one member of it left by checks before this one.
    const Type *now = targetType(flow, *target)->nonOptional();
    if (!now->isUnion() && !now->isObject())
      return false;
    llvm::StringRef name = member->name;
    auto tag = [&](const Type *m) -> const Type * {
      const Field *f = m->isObject() ? m->field(name) : nullptr;
      return f ? f->type : nullptr;
    };
    if (llvm::any_of(membersOf(now), [&](const Type *m) { return !tag(m); }))
      return false;
    narrow(flow, *target,
           [&](const Type *m) { return m->isUndefined() ? undefinedKept : keep(tag(m)); });
    return true;
  };
  // `if (r.ok)`, of `{ ok: true; value: int } | { ok: false; error: E }`.
  if (auto *member = dyn_cast<MemberExpr>(cond)) {
    auto mayBe = [](const Type *t, bool truthy) {
      if (!t->isLiteral())
        return true;
      bool value = t->isStringLiteral() ? !t->literalText().empty() : t->literalNumber() != 0;
      return value == truthy;
    };
    narrowByTag(member, whenTrue, [&](const Type *t) { return mayBe(t, true); });
    narrowByTag(member, whenFalse, [&](const Type *t) { return mayBe(t, false); });
    return;
  }
  auto *bin = dyn_cast<BinaryExpr>(cond);
  if (!bin)
    return;

  // `x instanceof C`: the members that are a C, and C itself if a member is
  // what C extends.
  if (bin->op == BinaryOp::InstanceOf) {
    const Type *cls = bin->rhs->type;
    std::optional<NarrowTarget> target = narrowTarget(bin->lhs);
    if (!target || !cls)
      return;
    // A class known only when the program runs: of a class T, as T.
    if (cls->isConstructor()) {
      cls = cls->constructed();
      if (!cls->isClass())
        return;
    }
    // `x instanceof Buffer`: a Buffer, or a Uint8Array that is one.
    if (cls->isBuffer()) {
      const Type *now = targetType(whenTrue, *target)->nonOptional();
      std::vector<const Type *> kept;
      for (const Type *m : membersOf(now))
        if (m->isBuffer() || m->bytes() == Type::Bytes::Uint8)
          kept.push_back(cls);
      narrowTargetTo(whenTrue, *target, kept.empty() ? nullptr : Type::unionOf(kept));
      narrow(whenFalse, *target, [&](const Type *m) { return !m->isBuffer(); });
      return;
    }
    // `x instanceof Date` (or Map, Set, Uint8Array...): the members Sema
    // gave the right side.
    if (!cls->isClass()) {
      const Type *those = cls;
      llvm::ArrayRef<const Type *> matching = membersOf(those);
      narrow(whenTrue, *target, [&](const Type *m) { return llvm::is_contained(matching, m); });
      narrow(whenFalse, *target, [&](const Type *m) { return !llvm::is_contained(matching, m); });
      return;
    }
    const Type *now = targetType(whenTrue, *target)->nonOptional();
    std::vector<const Type *> kept;
    for (const Type *m : membersOf(now)) {
      if (m->isObject() && m->isSubtypeOf(cls))
        kept.push_back(m);
      else if (m->isObject() && cls->isSubtypeOf(m))
        kept.push_back(cls);
    }
    narrowTargetTo(whenTrue, *target, kept.empty() ? nullptr : Type::unionOf(kept));
    narrow(whenFalse, *target,
           [&](const Type *m) { return !(m->isObject() && m->isSubtypeOf(cls)); });
    return;
  }

  // `"radius" in s`
  if (bin->op == BinaryOp::In) {
    std::optional<NarrowTarget> target = narrowTarget(bin->rhs);
    auto *key = dyn_cast<StringLitExpr>(bin->lhs);
    if (!target || !key)
      return;
    std::string name = toUtf8(key->value);
    auto has = [&](const Type *m) { return m->isObject() && m->field(name); };
    narrow(whenTrue, *target, has);
    narrow(whenFalse, *target, [&](const Type *m) { return !has(m) && !m->isUndefined(); });
    return;
  }

  if (bin->op != BinaryOp::Eq && bin->op != BinaryOp::Ne)
    return;
  Flow &equal = bin->op == BinaryOp::Eq ? whenTrue : whenFalse;
  Flow &different = bin->op == BinaryOp::Eq ? whenFalse : whenTrue;
  for (auto [side, other] : {std::pair{bin->lhs, bin->rhs}, std::pair{bin->rhs, bin->lhs}}) {
    // `typeof x == "string"`
    if (auto *typeOf = dyn_cast<UnaryExpr>(side); typeOf && typeOf->op == UnaryOp::TypeOf) {
      auto *text = dyn_cast<StringLitExpr>(other);
      std::optional<NarrowTarget> target = text ? narrowTarget(typeOf->operand) : std::nullopt;
      if (!target || !target->declared->nonOptional()->isUnion())
        continue;
      std::string kind = toUtf8(text->value);
      auto gives = [&](const Type *m) { return llvm::StringRef(typeOfName(m)) == kind; };
      narrow(equal, *target, gives);
      narrow(different, *target, [&](const Type *m) { return !gives(m); });
      continue;
    }
    const Type *literal = writtenLiteral(other);
    if (!literal)
      if (const Type *type = other->type; type && type->isLiteral())
        literal = type; // `s.kind == c.kind`, with c a Circle
    if (!literal)
      continue;
    // `s.kind == "circle"`: the members whose tag may be that; with
    // `s?.kind`, s is not undefined then.
    const auto *member = dyn_cast<MemberExpr>(side);
    if (auto *chain = dyn_cast<OptionalChainExpr>(side))
      member = dyn_cast<MemberExpr>(chain->chain);
    if (member && narrowByTag(
                      member, equal,
                      [&](const Type *t) { return isImplicitlyConvertible(literal, t); },
                      /*undefinedKept=*/false)) {
      narrowByTag(member, different, [&](const Type *t) { return t != literal; });
      continue;
    }
    // `x == "up"`, of a union holding that literal.
    if (std::optional<NarrowTarget> target = narrowTarget(side)) {
      const Type *now = targetType(equal, *target)->nonOptional();
      if (llvm::is_contained(membersOf(now), literal)) {
        narrowTargetTo(equal, *target, literal);
        narrow(different, *target, [&](const Type *m) { return m != literal; });
      } else if (now->isBool() && literal->literalBase()->isBool()) {
        narrowTargetTo(equal, *target, literal);
        narrowTargetTo(different, *target, Type::boolLiteral(literal->literalNumber() == 0));
      }
    }
  }
}

Expr *Sema::checkUnionMember(MemberExpr *member) {
  const Type *type = member->object->type;
  if (type->isTagged()) {
    diags_.error(member->loc(), "property '" + member->name + "' does not exist on '" +
                                    type->name() +
                                    "': check what it holds first, e.g. 'typeof x == \"" +
                                    typeOfName(type->members().front()) + "\"'");
    return member;
  }
  if (llvm::all_of(type->members(), [](const Type *m) { return m->isBytes(); }))
    return checkBytesUnionMember(member);
  std::vector<const Type *> fieldTypes;
  const Type *lacking = nullptr;
  for (const Type *m : type->members()) {
    // A class whose fields are not known yet, as no code used it before.
    if (ClassDecl *cls = classOf(m))
      completeClass(cls);
    const Field *f = m->isObject() ? m->field(member->name) : nullptr;
    if (const ClassDecl *cls = classOf(m)) {
      const ClassDecl *owner = nullptr;
      const ClassField *field = findField(cls, member->name, &owner);
      if (field && field->access != Access::Public &&
          !checkAccess(owner, field->access, member->name, member->loc()))
        return member;
      if (!field)
        f = nullptr;
    }
    if (!f) {
      lacking = m;
      break;
    }
    if (!llvm::is_contained(fieldTypes, f->type))
      fieldTypes.push_back(f->type);
  }
  if (lacking) {
    std::string message = "property '" + member->name.str() + "' does not exist on '" +
                          lacking->name().str() + "', which '" + type->name().str() +
                          "' may be; narrow it first";
    bool hinted = false;
    // The tag that tells the members apart, if they have one.
    for (const Field &f : type->members().front()->fields()) {
      bool tags = llvm::all_of(type->members(), [&](const Type *m) {
        const Field *g = m->field(f.name);
        return g && g->type->isLiteral();
      });
      if (!tags)
        continue;
      for (const Type *m : type->members())
        if (m->field(member->name)) {
          message += ", e.g. 'if (" + fieldText(member->object, f.name) +
                     " == " + m->field(f.name)->type->name().str() + ")'";
          hinted = true;
          break;
        }
      break;
    }
    if (!hinted) {
      const Type *with = nullptr;
      for (const Type *m : type->members())
        if (m->isObject() && m->field(member->name))
          with = m;
      if (with && with->isClass())
        message += ", e.g. 'if (x instanceof " + with->name().str() + ")'";
      else if (with)
        message += ", e.g. 'if (\"" + member->name.str() + "\" in x)'";
    }
    diags_.error(member->loc(), message);
    return member;
  }
  const Type *result = Type::unionOf(fieldTypes);
  if (std::string problem = unionProblem(result); !problem.empty()) {
    diags_.error(member->loc(),
                 "the members of '" + type->name() + "' have fields '" + member->name +
                     "' of types that one value cannot hold: " + problem + "; narrow it first");
    return member;
  }
  member->type = result;
  return member;
}

const Type *Sema::checkUnionFieldTarget(MemberExpr *member) {
  const Type *type = member->object->type;
  const Type *field = nullptr;
  for (const Type *m : type->members()) {
    if (ClassDecl *incomplete = classOf(m))
      completeClass(incomplete);
    const Field *f = m->isObject() ? m->field(member->name) : nullptr;
    const ClassDecl *cls = classOf(m);
    const ClassDecl *owner = nullptr;
    const ClassField *declared = cls ? findField(cls, member->name, &owner) : nullptr;
    if (!f || (cls && (!declared || declared->method))) {
      diags_.error(member->loc(), "property '" + member->name + "' does not exist on '" +
                                      m->name() + "', which '" + type->name() +
                                      "' may be; narrow it first");
      return Type::error();
    }
    if (declared && declared->access != Access::Public &&
        !checkAccess(owner, declared->access, member->name, member->loc()))
      return Type::error();
    if (f->readonly) {
      diags_.error(member->loc(), "cannot assign to '" + member->name +
                                      "' because it is a read-only field of '" + m->name() + "'");
      return Type::error();
    }
    if (field && f->type != field) {
      diags_.error(member->loc(), "the members of '" + type->name() + "' have fields '" +
                                      member->name + "' of different types, '" + field->name() +
                                      "' and '" + f->type->name() +
                                      "', so no value fits them all; narrow it first");
      return Type::error();
    }
    field = f->type;
  }
  fieldsAssigned(member->name);
  member->type = field;
  return field;
}

const Type *Sema::commonBaseWith(const Type *type, llvm::StringRef name) const {
  // The types up the declared chain of each member, nearest first.
  auto chain = [](const Type *t) {
    std::vector<const Type *> up{t};
    for (size_t i = 0; i < up.size(); ++i)
      for (const Type *b : up[i]->bases())
        if (!llvm::is_contained(up, b))
          up.push_back(b);
    return up;
  };
  llvm::ArrayRef<const Type *> members = type->members();
  if (llvm::any_of(members, [](const Type *m) { return !m->isObject(); }))
    return nullptr;
  for (const Type *candidate : chain(members.front())) {
    if (llvm::is_contained(members, candidate) && members.size() > 1)
      continue;
    bool all = llvm::all_of(members.drop_front(),
                            [&](const Type *m) { return m->isSubtypeOf(candidate); });
    if (!all)
      continue;
    if (const ClassDecl *cls = classOf(candidate)) {
      if (findField(cls, name) || !methodsNamed(cls, name, /*isStatic=*/false).empty())
        return candidate;
      continue;
    }
    if (candidate->field(name) || candidate->overload(name))
      return candidate;
  }
  return nullptr;
}

std::optional<std::vector<const Type *>> Sema::unhandledCases(const SwitchStmt *stmt) const {
  const Type *type = stmt->discriminant->type;
  if (!type || type->isError() || type->isOptional())
    return std::nullopt;
  if (!isLiteralSet(type))
    return std::nullopt;
  std::vector<const Type *> left(membersOf(type).begin(), membersOf(type).end());
  for (const SwitchCase &c : stmt->cases) {
    if (!c.test)
      return std::nullopt; // a default handles the rest
    if (const Type *literal = writtenLiteral(c.test))
      llvm::erase(left, literal);
    else if (c.test->type && c.test->type->isLiteral())
      llvm::erase(left, c.test->type);
  }
  return left;
}

Expr *Sema::checkUnionMethodCall(CallExpr *call, MemberExpr *member) {
  const Type *receiver = member->object->type;
  llvm::StringRef name = member->name;
  call->type = Type::error();
  member->type = Type::error();
  auto loosely = [&] {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
    return call;
  };
  if (receiver->isTagged() ||
      llvm::any_of(receiver->members(), [](const Type *m) { return !m->isObject(); })) {
    diags_.error(member->loc(), "property '" + name + "' does not exist on '" + receiver->name() +
                                    "': check what it holds first, e.g. 'typeof x == \"" +
                                    typeOfName(receiver->members().front()) + "\"'");
    return loosely();
  }
  const Type *fnType = nullptr;
  std::vector<CallExpr::UnionMethod> methods;
  for (const Type *m : receiver->members()) {
    const Type *type = nullptr;
    CallExpr::UnionMethod entry{m, nullptr, false};
    if (ClassDecl *cls = classOf(m)) {
      completeClass(cls);
      const ClassDecl *owner = nullptr;
      const ClassField *field = findField(cls, name, &owner);
      if (field && !field->method) { // a function in a field
        if (field->access != Access::Public &&
            !checkAccess(owner, field->access, name, member->loc()))
          return loosely();
        type = m->field(name) ? m->field(name)->type : nullptr;
      } else if (std::vector<FunctionDecl *> variants = methodsNamed(cls, name, /*isStatic=*/false);
                 variants.size() > 1) {
        diags_.error(member->loc(), "'" + name + "' of '" + m->name() +
                                        "' has several variants, which a call on '" +
                                        receiver->name() + "' cannot choose from; narrow it first");
        return loosely();
      } else if (!variants.empty()) {
        if (!checkAccess(variants[0]->cls, variants[0]->access, name, member->loc()))
          return loosely();
        FuncSymbol *sym = variants[0]->sym;
        resolveParams(sym);
        if (FunctionDecl *impl = soleImplementation(cls, sym->decl))
          entry.method = impl->sym;
        else
          entry.method = sym, entry.virtualCall = true;
        type = functionType(sym);
      }
    } else if (const Field *f = m->field(name); f && m->isBase()) {
      // An interface that classes implement: its methods are in their
      // tables, which a call on the union does not reach.
      diags_.error(member->loc(), "'" + name + "' is called on '" + receiver->name() +
                                      "', and '" + m->name() +
                                      "' is an interface that classes implement; narrow it "
                                      "first, or declare the members to be one type with '" +
                                      name + "'");
      return loosely();
    } else if (f) {
      type = f->type;
    }
    if (!type || !type->isFunction()) {
      diags_.error(member->loc(), "'" + receiver->name() + "' may be a '" + m->name() + "', " +
                                      (type ? "whose '" + name.str() + "' is not a function"
                                            : "which has no method '" + name.str() + "'") +
                                      "; narrow it first");
      return loosely();
    }
    if (fnType && type != fnType) {
      diags_.error(member->loc(), "the members of '" + receiver->name() + "' have methods '" +
                                      name + "' of different types, '" + fnType->name() +
                                      "' and '" + type->name() + "'; narrow it first");
      return loosely();
    }
    fnType = type;
    methods.push_back(entry);
  }
  call->unionMethods = std::move(methods);
  member->type = fnType;
  checkArguments(call, fnType->params(), fnType->minArgs(), fnType->hasRest(), member->loc());
  call->type = fnType->ret();
  return call;
}

} // namespace tz
