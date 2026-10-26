#include "compiler/check/flow/union_narrowing/union_narrowing.h"
#include "compiler/check/flow/field_facts/field_facts.h"
#include "compiler/check/types/literal_types/literal_types.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/union_check_helpers/union_check_helpers.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
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

llvm::ArrayRef<const Type *> membersOf(const Type *&type) {
  return type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
}

const VarSymbol *readVariable(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit() || conv->shortCircuit) {
      return nullptr;
    }
    expr = conv->operand;
  }
  auto *name = dyn_cast<NameExpr>(expr);

  return name ? name->var : nullptr;
}

}

static const Type *keepMembers(const Type *type, llvm::function_ref<bool(const Type *)> keep) {
  bool undefined = type->isOptional();
  const Type *value = type->nonOptional();
  std::vector<const Type *> kept;
  for (const Type *m : membersOf(value)) {
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

UnionNarrowing::UnionNarrowing(Checker &checker) : checker_(checker) {}

void UnionNarrowing::narrowVariable(Checker::Flow &flow, const VarSymbol *var, const Type *type) {
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

std::optional<Checker::NarrowTarget> UnionNarrowing::narrowTarget(const Expr *expr) const {
  const Type *declared = nullptr;
  Checker::NarrowTarget target;
  if (const VarSymbol *var = readVariable(expr); var && var->type) {
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
    std::optional<Checker::FieldPath> path =
        member ? checker_.fieldFacts().fieldPath(member) : std::nullopt;
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

const Type *UnionNarrowing::targetType(const Checker::Flow &flow,
                                       const Checker::NarrowTarget &target) const {
  if (target.var) {
    return checker_.literalTypes().typeIn(flow, target.var);
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

void UnionNarrowing::narrowTargetTo(Checker::Flow &flow, const Checker::NarrowTarget &target,
                                    const Type *type) const {
  if (target.var) {
    narrowVariable(flow, target.var, type);
    return;
  }
  const Checker::FieldPath &path = *target.path;
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

void UnionNarrowing::addUnionFacts(const Expr *cond, Checker::Flow &whenTrue,
                                   Checker::Flow &whenFalse) const {
  auto narrow = [&](Checker::Flow &flow, const Checker::NarrowTarget &target,
                    llvm::function_ref<bool(const Type *)> keep) {
    const Type *now = targetType(flow, target);
    if (now->isNever()) {
      return;
    }
    narrowTargetTo(flow, target, keepMembers(now, keep));
  };

  if (auto *call = dyn_cast<CallExpr>(cond); call && call->builtin == Builtin::ArrayIsArray) {
    if (std::optional<Checker::NarrowTarget> target = narrowTarget(call->args[0])) {
      narrow(whenTrue, *target, [](const Type *m) { return m->isArray(); });
      narrow(whenFalse, *target, [](const Type *m) { return !m->isArray(); });
    }
    return;
  }
  auto narrowByTag = [&](const MemberExpr *member, Checker::Flow &flow,
                         llvm::function_ref<bool(const Type *)> keep, bool undefinedKept = true) {
    const Expr *object = member->object;
    if (member->optional) {
      auto *link = dyn_cast<CastExpr>(object);
      if (!link || !link->shortCircuit) {
        return false;
      }
      object = link->operand;
    }
    std::optional<Checker::NarrowTarget> target = narrowTarget(object);
    if (!target || !target->declared->nonOptional()->isUnion()) {
      return false;
    }
    const Type *now = targetType(flow, *target)->nonOptional();
    if (!now->isUnion() && !now->isObject()) {
      return false;
    }
    llvm::StringRef name = member->name;
    auto tag = [&](const Type *m) -> const Type * {
      const Field *f = m->isObject() ? m->field(name) : nullptr;

      return f ? f->type : nullptr;
    };
    if (llvm::any_of(membersOf(now), [&](const Type *m) { return !tag(m); })) {
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
    std::optional<Checker::NarrowTarget> target = narrowTarget(bin->lhs);
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
      const Type *now = targetType(whenTrue, *target)->nonOptional();
      std::vector<const Type *> kept;
      for (const Type *m : membersOf(now)) {
        if (m->isBuffer() || m->bytes() == Type::Bytes::Uint8) {
          kept.push_back(cls);
        }
      }
      narrowTargetTo(whenTrue, *target, kept.empty() ? nullptr : Type::unionOf(kept));
      narrow(whenFalse, *target, [&](const Type *m) { return !m->isBuffer(); });
      return;
    }
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
      if (m->isObject() && m->isSubtypeOf(cls)) {
        kept.push_back(m);
      } else if (m->isObject() && cls->isSubtypeOf(m)) {
        kept.push_back(cls);
      }
    }
    narrowTargetTo(whenTrue, *target, kept.empty() ? nullptr : Type::unionOf(kept));
    narrow(whenFalse, *target,
           [&](const Type *m) { return !(m->isObject() && m->isSubtypeOf(cls)); });
    return;
  }

  if (bin->op == BinaryOp::In) {
    std::optional<Checker::NarrowTarget> target = narrowTarget(bin->rhs);
    auto *key = dyn_cast<StringLitExpr>(bin->lhs);
    if (!target || !key) {
      return;
    }
    std::string name = toUtf8(key->value);
    auto has = [&](const Type *m) { return m->isObject() && m->field(name); };
    narrow(whenTrue, *target, has);
    narrow(whenFalse, *target, [&](const Type *m) { return !has(m) && !m->isUndefined(); });
    return;
  }

  if (bin->op != BinaryOp::Eq && bin->op != BinaryOp::Ne) {
    return;
  }
  Checker::Flow &equal = bin->op == BinaryOp::Eq ? whenTrue : whenFalse;
  Checker::Flow &different = bin->op == BinaryOp::Eq ? whenFalse : whenTrue;
  for (auto [side, other] : {std::pair{bin->lhs, bin->rhs}, std::pair{bin->rhs, bin->lhs}}) {
    if (auto *typeOf = dyn_cast<UnaryExpr>(side); typeOf && typeOf->op == UnaryOp::TypeOf) {
      auto *text = dyn_cast<StringLitExpr>(other);
      std::optional<Checker::NarrowTarget> target =
          text ? narrowTarget(typeOf->operand) : std::nullopt;
      if (!target || !target->declared->nonOptional()->isUnion()) {
        continue;
      }
      std::string kind = toUtf8(text->value);
      auto gives = [&](const Type *m) { return llvm::StringRef(typeOfName(m)) == kind; };
      narrow(equal, *target, gives);
      narrow(different, *target, [&](const Type *m) { return !gives(m); });
      continue;
    }
    const Type *literal = writtenLiteral(other);
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
    if (std::optional<Checker::NarrowTarget> target = narrowTarget(side)) {
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

}
