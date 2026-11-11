#include "compiler/semantic/flow/union_narrowing/union_narrowing.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/flow/field_facts/field_facts.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/union_check_helpers/union_check_helpers.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::dyn_cast;

namespace tz {

UnionNarrowing::UnionNarrowing(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->fieldFacts = &all.fieldFacts;
  this->literalTypes = &all.literalTypes;
}

llvm::ArrayRef<const Type *> UnionNarrowing::membersOf(const Type *&type) {
  return type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
}

const VarSymbol *UnionNarrowing::readVariable(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit() || conv->shortCircuit) {
      return nullptr;
    }
    expr = conv->operand;
  }
  auto *name = dyn_cast<NameExpr>(expr);

  return name ? name->var() : nullptr;
}

const Type *UnionNarrowing::keepMembers(const Type *type,
                                        llvm::function_ref<bool(const Type *)> keep) {
  bool undefined = type->isOptional();
  const Type *value = type->nonOptional();
  std::vector<const Type *> kept;
  for (const Type *m : UnionNarrowing::membersOf(value)) {
    if (keep(m)) {
      kept.push_back(m);
    }
  }
  bool keepsUndefined = undefined && keep(Type::undefinedTy());
  if (kept.empty()) {
    return keepsUndefined ? type : nullptr;
  }
  const Type *result = Type::unionOf(kept);

  return keepsUndefined ? Type::optional(result) : result;
}

void UnionNarrowing::narrowVariable(Flow &flow, const VarSymbol *var, const Type *type) {
  llvm::erase_if(flow.narrowed, [&](const auto &narrowed) { return narrowed.first == var; });
  if (!type) {
    flow.narrowed.push_back({var, Type::never()});
    return;
  }
  if (type == var->type) {
    return;
  }
  if (var->type->isOptional() && type == var->type->inner()) {
    flow.defined.insert(var);
    return;
  }
  flow.narrowed.push_back({var, type});
  if (!type->isOptional()) {
    flow.defined.insert(var);
  }
}

std::optional<NarrowTarget> UnionNarrowing::narrowTarget(const Expr *expr) const {
  const Type *declared = nullptr;
  NarrowTarget target;
  if (const VarSymbol *var = UnionNarrowing::readVariable(expr); var && var->type) {
    target.var = var;
    declared = var->type;
  } else {
    const Expr *read = expr;
    while (auto *conv = dyn_cast<CastExpr>(read)) {
      if (!conv->isImplicit() || conv->shortCircuit) {
        return std::nullopt;
      }
      read = conv->operand;
    }
    auto *member = dyn_cast<MemberExpr>(read);
    std::optional<FieldPath> path = member ? this->fieldFacts->fieldPath(member) : std::nullopt;
    if (!path) {
      return std::nullopt;
    }
    target.path = std::move(*path);
    declared = member->type;
  }
  const Type *base = declared->nonOptional();
  if (!base->isUnion() && !base->isLiteral() && !base->isBool()) {
    return std::nullopt;
  }
  target.declared = declared;

  return target;
}

const Type *UnionNarrowing::targetType(const Flow &flow, const NarrowTarget &target) const {
  if (target.var) {
    return this->literalTypes->typeIn(flow, target.var);
  }
  for (const auto &[path, type] : flow.narrowedFields) {
    if (path == *target.path) {
      return type;
    }
  }
  if (target.declared->isOptional() && llvm::is_contained(flow.definedFields, *target.path)) {
    return target.declared->inner();
  }

  return target.declared;
}

void UnionNarrowing::narrowTargetTo(Flow &flow, const NarrowTarget &target,
                                    const Type *type) const {
  if (target.var) {
    UnionNarrowing::narrowVariable(flow, target.var, type);
    return;
  }
  const FieldPath &path = *target.path;
  llvm::erase_if(flow.narrowedFields, [&](const auto &entry) { return entry.first == path; });
  if (type && !type->isOptional()) {
    FieldFacts::defineField(flow, path);
  }
  if (type == target.declared ||
      (type && target.declared->isOptional() && type == target.declared->inner())) {
    return;
  }
  flow.narrowedFields.push_back({path, type ? type : Type::never()});
}

void UnionNarrowing::addUnionFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const {
  auto *call = dyn_cast<CallExpr>(cond);
  if (call != nullptr && call->builtin() == Builtin::ArrayIsArray) {
    this->addArrayFacts(call->args[0], whenTrue, whenFalse);
    return;
  }
  auto *member = dyn_cast<MemberExpr>(cond);
  if (member != nullptr) {
    this->narrowByTag(member, whenTrue, UnionNarrowing::mayBeTruthy, true);
    this->narrowByTag(member, whenFalse, UnionNarrowing::mayBeFalsy, true);
    return;
  }
  auto *bin = dyn_cast<BinaryExpr>(cond);
  if (bin == nullptr) {
    return;
  }
  if (bin->op == BinaryOp::InstanceOf) {
    this->addInstanceOfFacts(bin, whenTrue, whenFalse);
    return;
  }
  if (bin->op == BinaryOp::In) {
    this->addInFacts(bin, whenTrue, whenFalse);
    return;
  }
  if (bin->op != BinaryOp::Eq && bin->op != BinaryOp::Ne) {
    return;
  }
  Flow &equal = bin->op == BinaryOp::Eq ? whenTrue : whenFalse;
  Flow &different = bin->op == BinaryOp::Eq ? whenFalse : whenTrue;
  for (auto [side, other] : {std::pair{bin->lhs, bin->rhs}, std::pair{bin->rhs, bin->lhs}}) {
    this->addEqualityFacts(side, other, equal, different);
  }
}

void UnionNarrowing::narrowKept(Flow &flow, const NarrowTarget &target,
                                llvm::function_ref<bool(const Type *)> keep) const {
  const Type *now = this->targetType(flow, target);
  if (now->isNever()) {
    return;
  }
  const Type *kept = UnionNarrowing::keepMembers(now, keep);
  this->narrowTargetTo(flow, target, kept);
}

const Type *UnionNarrowing::tagOf(const Type *m, llvm::StringRef name) {
  const Field *f = m->isObject() ? m->field(name) : nullptr;

  return f != nullptr ? f->type : nullptr;
}

bool UnionNarrowing::allTagged(const Type *now, llvm::StringRef name) {
  for (const Type *m : UnionNarrowing::membersOf(now)) {
    if (UnionNarrowing::tagOf(m, name) == nullptr) {
      return false;
    }
  }

  return true;
}

bool UnionNarrowing::narrowByTag(const MemberExpr *member, Flow &flow,
                                 llvm::function_ref<bool(const Type *)> keep,
                                 bool undefinedKept) const {
  const Expr *object = member->object;
  if (member->optional) {
    auto *link = dyn_cast<CastExpr>(object);
    if (link == nullptr || !link->shortCircuit) {
      return false;
    }
    object = link->operand;
  }
  std::optional<NarrowTarget> target = this->narrowTarget(object);
  if (!target.has_value() || !target->declared->nonOptional()->isUnion()) {
    return false;
  }
  const Type *now = this->targetType(flow, *target)->nonOptional();
  if (!now->isUnion() && !now->isObject()) {
    return false;
  }
  llvm::StringRef name = member->name;
  if (!UnionNarrowing::allTagged(now, name)) {
    return false;
  }
  this->narrowKept(flow, *target, [&](const Type *m) {
    return m->isUndefined() ? undefinedKept : keep(UnionNarrowing::tagOf(m, name));
  });

  return true;
}

bool UnionNarrowing::mayBe(const Type *t, bool truthy) {
  if (!t->isLiteral()) {
    return true;
  }
  bool value = t->isStringLiteral() ? !t->literalText().empty() : t->literalNumber() != 0;

  return value == truthy;
}

bool UnionNarrowing::mayBeTruthy(const Type *t) {
  return UnionNarrowing::mayBe(t, true);
}

bool UnionNarrowing::mayBeFalsy(const Type *t) {
  return UnionNarrowing::mayBe(t, false);
}

bool UnionNarrowing::isArrayMember(const Type *m) {
  return m->isArray();
}

bool UnionNarrowing::isNotArrayMember(const Type *m) {
  return !m->isArray();
}

bool UnionNarrowing::isNotBufferMember(const Type *m) {
  return !m->isBuffer();
}

void UnionNarrowing::addArrayFacts(const Expr *arg, Flow &whenTrue, Flow &whenFalse) const {
  std::optional<NarrowTarget> target = this->narrowTarget(arg);
  if (!target.has_value()) {
    return;
  }
  this->narrowKept(whenTrue, *target, UnionNarrowing::isArrayMember);
  this->narrowKept(whenFalse, *target, UnionNarrowing::isNotArrayMember);
}

void UnionNarrowing::addInstanceOfFacts(const BinaryExpr *bin, Flow &whenTrue,
                                        Flow &whenFalse) const {
  const Type *cls = bin->rhs->type;
  std::optional<NarrowTarget> target = this->narrowTarget(bin->lhs);
  if (!target.has_value() || cls == nullptr) {
    return;
  }
  if (cls->isConstructor()) {
    cls = cls->constructed();
    if (!cls->isClass()) {
      return;
    }
  }
  if (cls->isBuffer()) {
    this->addBufferFacts(*target, cls, whenTrue, whenFalse);
    return;
  }
  if (!cls->isClass()) {
    this->addMemberSetFacts(*target, cls, whenTrue, whenFalse);
    return;
  }
  this->addClassFacts(*target, cls, whenTrue, whenFalse);
}

void UnionNarrowing::addBufferFacts(const NarrowTarget &target, const Type *cls, Flow &whenTrue,
                                    Flow &whenFalse) const {
  const Type *now = this->targetType(whenTrue, target)->nonOptional();
  std::vector<const Type *> kept;
  for (const Type *m : UnionNarrowing::membersOf(now)) {
    if (m->isBuffer() || m->bytes() == Type::Bytes::Uint8) {
      kept.push_back(cls);
    }
  }
  this->narrowTargetTo(whenTrue, target, kept.empty() ? nullptr : Type::unionOf(kept));
  this->narrowKept(whenFalse, target, UnionNarrowing::isNotBufferMember);
}

void UnionNarrowing::addMemberSetFacts(const NarrowTarget &target, const Type *cls, Flow &whenTrue,
                                       Flow &whenFalse) const {
  const Type *those = cls;
  llvm::ArrayRef<const Type *> matching = UnionNarrowing::membersOf(those);
  this->narrowKept(whenTrue, target,
                   [&](const Type *m) { return llvm::is_contained(matching, m); });
  this->narrowKept(whenFalse, target,
                   [&](const Type *m) { return !llvm::is_contained(matching, m); });
}

void UnionNarrowing::addClassFacts(const NarrowTarget &target, const Type *cls, Flow &whenTrue,
                                   Flow &whenFalse) const {
  const Type *now = this->targetType(whenTrue, target)->nonOptional();
  std::vector<const Type *> kept;
  for (const Type *m : UnionNarrowing::membersOf(now)) {
    if (m->isObject() && m->isSubtypeOf(cls)) {
      kept.push_back(m);
    } else if (m->isObject() && cls->isSubtypeOf(m)) {
      kept.push_back(cls);
    }
  }
  this->narrowTargetTo(whenTrue, target, kept.empty() ? nullptr : Type::unionOf(kept));
  this->narrowKept(whenFalse, target,
                   [&](const Type *m) { return !(m->isObject() && m->isSubtypeOf(cls)); });
}

bool UnionNarrowing::hasField(const Type *m, llvm::StringRef name) {
  return m->isObject() && m->field(name) != nullptr;
}

void UnionNarrowing::addInFacts(const BinaryExpr *bin, Flow &whenTrue, Flow &whenFalse) const {
  std::optional<NarrowTarget> target = this->narrowTarget(bin->rhs);
  auto *key = dyn_cast<StringLitExpr>(bin->lhs);
  if (!target.has_value() || key == nullptr) {
    return;
  }
  std::string name = ToUtf8::toUtf8(key->value);
  this->narrowKept(whenTrue, *target,
                   [&](const Type *m) { return UnionNarrowing::hasField(m, name); });
  this->narrowKept(whenFalse, *target, [&](const Type *m) {
    return !UnionNarrowing::hasField(m, name) && !m->isUndefined();
  });
}

void UnionNarrowing::addEqualityFacts(const Expr *side, const Expr *other, Flow &equal,
                                      Flow &different) const {
  auto *typeOf = dyn_cast<UnaryExpr>(side);
  if (typeOf != nullptr && typeOf->op == UnaryOp::TypeOf) {
    this->addTypeOfFacts(typeOf, other, equal, different);
    return;
  }
  const Type *literal = UnionNarrowing::comparedLiteral(other);
  if (literal != nullptr) {
    this->addLiteralFacts(side, literal, equal, different);
  }
}

bool UnionNarrowing::givesTypeOf(const Type *m, llvm::StringRef kind) {
  return llvm::StringRef(TypeSpelling::typeOfName(m)) == kind;
}

void UnionNarrowing::addTypeOfFacts(const UnaryExpr *typeOf, const Expr *other, Flow &equal,
                                    Flow &different) const {
  auto *text = dyn_cast<StringLitExpr>(other);
  std::optional<NarrowTarget> target =
      text != nullptr ? this->narrowTarget(typeOf->operand) : std::nullopt;
  if (!target.has_value() || !target->declared->nonOptional()->isUnion()) {
    return;
  }
  std::string kind = ToUtf8::toUtf8(text->value);
  this->narrowKept(equal, *target,
                   [&](const Type *m) { return UnionNarrowing::givesTypeOf(m, kind); });
  this->narrowKept(different, *target,
                   [&](const Type *m) { return !UnionNarrowing::givesTypeOf(m, kind); });
}

const Type *UnionNarrowing::comparedLiteral(const Expr *other) {
  const Type *literal = UnionCheckHelpers::writtenLiteral(other);
  if (literal != nullptr) {
    return literal;
  }
  const Type *type = other->type;

  return type != nullptr && type->isLiteral() ? type : nullptr;
}

const MemberExpr *UnionNarrowing::taggedMember(const Expr *side) {
  auto *chain = dyn_cast<OptionalChainExpr>(side);
  if (chain != nullptr) {
    return dyn_cast<MemberExpr>(chain->chain);
  }

  return dyn_cast<MemberExpr>(side);
}

void UnionNarrowing::addLiteralFacts(const Expr *side, const Type *literal, Flow &equal,
                                     Flow &different) const {
  const MemberExpr *member = UnionNarrowing::taggedMember(side);
  bool tagged =
      member != nullptr &&
      this->narrowByTag(
          member, equal,
          [&](const Type *t) { return TypeRelations::isImplicitlyConvertible(literal, t); }, false);
  if (tagged) {
    this->narrowByTag(member, different, [&](const Type *t) { return t != literal; }, true);
    return;
  }
  std::optional<NarrowTarget> target = this->narrowTarget(side);
  if (target.has_value()) {
    this->narrowToLiteral(*target, literal, equal, different);
    return;
  }
  const VarSymbol *var = UnionNarrowing::readVariable(side);
  bool narrows = var != nullptr && var->type != nullptr &&
                 literal->literalBase() == var->type->nonOptional() &&
                 (literal->isStringLiteral() || literal->literalBase()->isInt()) &&
                 this->literalTypes->typeIn(equal, var) == var->type;
  if (narrows) {
    UnionNarrowing::narrowVariable(equal, var, literal);
  }
}

void UnionNarrowing::narrowToLiteral(const NarrowTarget &target, const Type *literal, Flow &equal,
                                     Flow &different) const {
  const Type *now = this->targetType(equal, target)->nonOptional();
  if (llvm::is_contained(UnionNarrowing::membersOf(now), literal)) {
    this->narrowTargetTo(equal, target, literal);
    this->narrowKept(different, target, [&](const Type *m) { return m != literal; });
    return;
  }
  if (now->isBool() && literal->literalBase()->isBool()) {
    this->narrowTargetTo(equal, target, literal);
    const Type *opposite = Type::boolLiteral(literal->literalNumber() == 0);
    this->narrowTargetTo(different, target, opposite);
  }
}
}
