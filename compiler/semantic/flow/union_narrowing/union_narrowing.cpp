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
  this->fieldFacts = &all.getFieldFacts();
  this->literalTypes = &all.getLiteralTypes();
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
  auto narrow = [&](Flow &flow, const NarrowTarget &target,
                    llvm::function_ref<bool(const Type *)> keep) {
    const Type *now = this->targetType(flow, target);
    if (now->isNever()) {
      return;
    }
    this->narrowTargetTo(flow, target, UnionNarrowing::keepMembers(now, keep));
  };

  if (auto *call = dyn_cast<CallExpr>(cond); call && call->builtin() == Builtin::ArrayIsArray) {
    if (std::optional<NarrowTarget> target = this->narrowTarget(call->args[0])) {
      narrow(whenTrue, *target, [](const Type *m) { return m->isArray(); });
      narrow(whenFalse, *target, [](const Type *m) { return !m->isArray(); });
    }
    return;
  }
  auto narrowByTag = [&](const MemberExpr *member, Flow &flow,
                         llvm::function_ref<bool(const Type *)> keep, bool undefinedKept = true) {
    const Expr *object = member->object;
    if (member->optional) {
      auto *link = dyn_cast<CastExpr>(object);
      if (!link || !link->shortCircuit) {
        return false;
      }
      object = link->operand;
    }
    std::optional<NarrowTarget> target = this->narrowTarget(object);
    if (!target || !target->declared->nonOptional()->isUnion()) {
      return false;
    }
    const Type *now = this->targetType(flow, *target)->nonOptional();
    if (!now->isUnion() && !now->isObject()) {
      return false;
    }
    llvm::StringRef name = member->name;
    auto tag = [&](const Type *m) -> const Type * {
      const Field *f = m->isObject() ? m->field(name) : nullptr;

      return f ? f->type : nullptr;
    };
    if (llvm::any_of(UnionNarrowing::membersOf(now), [&](const Type *m) { return !tag(m); })) {
      return false;
    }
    narrow(flow, *target,
           [&](const Type *m) { return m->isUndefined() ? undefinedKept : keep(tag(m)); });

    return true;
  };
  if (auto *member = dyn_cast<MemberExpr>(cond)) {
    auto mayBe = [](const Type *t, bool truthy) {
      if (!t->isLiteral()) {
        return true;
      }
      bool value = t->isStringLiteral() ? !t->literalText().empty() : t->literalNumber() != 0;

      return value == truthy;
    };
    narrowByTag(member, whenTrue, [&](const Type *t) { return mayBe(t, true); });
    narrowByTag(member, whenFalse, [&](const Type *t) { return mayBe(t, false); });
    return;
  }
  auto *bin = dyn_cast<BinaryExpr>(cond);
  if (!bin) {
    return;
  }

  if (bin->op == BinaryOp::InstanceOf) {
    const Type *cls = bin->rhs->type;
    std::optional<NarrowTarget> target = this->narrowTarget(bin->lhs);
    if (!target || !cls) {
      return;
    }
    if (cls->isConstructor()) {
      cls = cls->constructed();
      if (!cls->isClass()) {
        return;
      }
    }
    if (cls->isBuffer()) {
      const Type *now = this->targetType(whenTrue, *target)->nonOptional();
      std::vector<const Type *> kept;
      for (const Type *m : UnionNarrowing::membersOf(now)) {
        if (m->isBuffer() || m->bytes() == Type::Bytes::Uint8) {
          kept.push_back(cls);
        }
      }
      this->narrowTargetTo(whenTrue, *target, kept.empty() ? nullptr : Type::unionOf(kept));
      narrow(whenFalse, *target, [&](const Type *m) { return !m->isBuffer(); });
      return;
    }
    if (!cls->isClass()) {
      const Type *those = cls;
      llvm::ArrayRef<const Type *> matching = UnionNarrowing::membersOf(those);
      narrow(whenTrue, *target, [&](const Type *m) { return llvm::is_contained(matching, m); });
      narrow(whenFalse, *target, [&](const Type *m) { return !llvm::is_contained(matching, m); });
      return;
    }
    const Type *now = this->targetType(whenTrue, *target)->nonOptional();
    std::vector<const Type *> kept;
    for (const Type *m : UnionNarrowing::membersOf(now)) {
      if (m->isObject() && m->isSubtypeOf(cls)) {
        kept.push_back(m);
      } else if (m->isObject() && cls->isSubtypeOf(m)) {
        kept.push_back(cls);
      }
    }
    this->narrowTargetTo(whenTrue, *target, kept.empty() ? nullptr : Type::unionOf(kept));
    narrow(whenFalse, *target,
           [&](const Type *m) { return !(m->isObject() && m->isSubtypeOf(cls)); });
    return;
  }

  if (bin->op == BinaryOp::In) {
    std::optional<NarrowTarget> target = this->narrowTarget(bin->rhs);
    auto *key = dyn_cast<StringLitExpr>(bin->lhs);
    if (!target || !key) {
      return;
    }
    std::string name = ToUtf8::toUtf8(key->value);
    auto has = [&](const Type *m) { return m->isObject() && m->field(name); };
    narrow(whenTrue, *target, has);
    narrow(whenFalse, *target, [&](const Type *m) { return !has(m) && !m->isUndefined(); });
    return;
  }

  if (bin->op != BinaryOp::Eq && bin->op != BinaryOp::Ne) {
    return;
  }
  Flow &equal = bin->op == BinaryOp::Eq ? whenTrue : whenFalse;
  Flow &different = bin->op == BinaryOp::Eq ? whenFalse : whenTrue;
  for (auto [side, other] : {std::pair{bin->lhs, bin->rhs}, std::pair{bin->rhs, bin->lhs}}) {
    if (auto *typeOf = dyn_cast<UnaryExpr>(side); typeOf && typeOf->op == UnaryOp::TypeOf) {
      auto *text = dyn_cast<StringLitExpr>(other);
      std::optional<NarrowTarget> target =
          text ? this->narrowTarget(typeOf->operand) : std::nullopt;
      if (!target || !target->declared->nonOptional()->isUnion()) {
        continue;
      }
      std::string kind = ToUtf8::toUtf8(text->value);
      auto gives = [&](const Type *m) {
        return llvm::StringRef(TypeSpelling::typeOfName(m)) == kind;
      };
      narrow(equal, *target, gives);
      narrow(different, *target, [&](const Type *m) { return !gives(m); });
      continue;
    }
    const Type *literal = UnionCheckHelpers::writtenLiteral(other);
    if (!literal) {
      if (const Type *type = other->type; type && type->isLiteral()) {
        literal = type;
      }
    }
    if (!literal) {
      continue;
    }
    const auto *member = dyn_cast<MemberExpr>(side);
    if (auto *chain = dyn_cast<OptionalChainExpr>(side)) {
      member = dyn_cast<MemberExpr>(chain->chain);
    }
    if (member &&
        narrowByTag(
            member, equal,
            [&](const Type *t) { return TypeRelations::isImplicitlyConvertible(literal, t); },
            false)) {
      narrowByTag(member, different, [&](const Type *t) { return t != literal; });
      continue;
    }
    if (std::optional<NarrowTarget> target = this->narrowTarget(side)) {
      const Type *now = this->targetType(equal, *target)->nonOptional();
      if (llvm::is_contained(UnionNarrowing::membersOf(now), literal)) {
        this->narrowTargetTo(equal, *target, literal);
        narrow(different, *target, [&](const Type *m) { return m != literal; });
      } else if (now->isBool() && literal->literalBase()->isBool()) {
        this->narrowTargetTo(equal, *target, literal);
        this->narrowTargetTo(different, *target, Type::boolLiteral(literal->literalNumber() == 0));
      }
    } else if (const VarSymbol *var = UnionNarrowing::readVariable(side);
               var && var->type && literal->literalBase() == var->type->nonOptional() &&
               (literal->isStringLiteral() || literal->literalBase()->isInt()) &&
               this->literalTypes->typeIn(equal, var) == var->type) {
      UnionNarrowing::narrowVariable(equal, var, literal);
    }
  }
}

}
