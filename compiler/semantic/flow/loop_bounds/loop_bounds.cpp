#include "compiler/semantic/flow/loop_bounds/loop_bounds.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"
#include <limits>

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr int64_t kIntMin = std::numeric_limits<int64_t>::min();

constexpr int64_t kIntMax = std::numeric_limits<int64_t>::max();

constexpr uint64_t kMaxConstant = uint64_t(1) << 40;

}

LoopBounds::LoopBounds(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->flowChecker = &all.flowChecker;
}

std::optional<int64_t>
LoopBounds::varPlus(const Expr *expr, const VarSymbol *var,
                    llvm::function_ref<const VarSymbol *(llvm::StringRef)> lookupVar) {
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    return lookupVar(name->name) == var ? std::optional<int64_t>(0) : std::nullopt;
  }
  auto *bin = dyn_cast<BinaryExpr>(expr);
  if (!bin || bin->op != BinaryOp::Add) {
    return std::nullopt;
  }
  const Expr *other = bin->rhs;
  auto *name = dyn_cast<NameExpr>(bin->lhs);
  if (!name) {
    name = dyn_cast<NameExpr>(bin->rhs);
    other = bin->lhs;
  }
  auto *lit = dyn_cast<IntLitExpr>(other);
  if (!name || !lit || lookupVar(name->name) != var || lit->value > uint64_t(Type::kMaxLength)) {
    return std::nullopt;
  }

  return static_cast<int64_t>(lit->value);
}

bool LoopBounds::isArrayLength(const Term &t) {
  return t.length && t.var->type->nonOptional()->isArray();
}

std::optional<std::pair<int64_t, int64_t>> LoopBounds::syntacticIntRange(const Expr *expr) const {
  if (auto *lit = dyn_cast<IntLitExpr>(expr)) {
    if (lit->value > uint64_t(kIntMax)) {
      return std::nullopt;
    }
    auto v = static_cast<int64_t>(lit->value);
    return std::pair{v, v};
  }
  if (auto *unary = dyn_cast<UnaryExpr>(expr)) {
    auto *lit = dyn_cast<IntLitExpr>(unary->operand);
    if (unary->op != UnaryOp::Neg || !lit || lit->value > uint64_t(kIntMax)) {
      return std::nullopt;
    }
    auto v = -static_cast<int64_t>(lit->value);
    return std::pair{v, v};
  }
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    const VarSymbol *var = this->scopes->lookup(name->name).var();
    if (var && var->type->isInt()) {
      return std::pair{kIntMin, kIntMax};
    }
    return std::nullopt;
  }
  if (auto *member = dyn_cast<MemberExpr>(expr)) {
    return this->memberRange(member);
  }
  if (auto *bin = dyn_cast<BinaryExpr>(expr)) {
    auto *member = dyn_cast<MemberExpr>(bin->lhs);
    auto *lit = dyn_cast<IntLitExpr>(bin->rhs);
    if (bin->op != BinaryOp::Sub || member == nullptr || !lit || lit->value > kMaxConstant) {
      return std::nullopt;
    }
    std::optional<std::pair<int64_t, int64_t>> length = this->memberRange(member);
    if (!length) {
      return std::nullopt;
    }
    auto k = static_cast<int64_t>(lit->value);
    return std::pair{length->first - k, length->second - k};
  }

  return std::nullopt;
}

std::optional<std::pair<int64_t, int64_t>> LoopBounds::memberRange(const MemberExpr *member) const {
  auto *name = dyn_cast<NameExpr>(member->object);
  const VarSymbol *var = name ? this->scopes->lookup(name->name).var() : nullptr;
  if (BuiltinPropertyTable::isName(member->name, BuiltinProperty::BYTE_LENGTH) && var &&
      var->type->isBytes()) {
    return std::pair{int64_t(0), Type::kMaxLength};
  }
  if (!BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH)) {
    return std::nullopt;
  }
  if (isa<StringLitExpr>(member->object) || isa<ArrayLitExpr>(member->object)) {
    return std::pair{int64_t(0), Type::kMaxLength};
  }
  if (var && (var->type->isString() || var->type->isArray() || var->type->isTypedArray())) {
    return std::pair{int64_t(0), Type::kMaxLength};
  }

  return std::nullopt;
}

bool LoopBounds::boundedAbove(const Expr *expr, int64_t limit, const LoopScan &scan) const {
  if (std::optional<std::pair<int64_t, int64_t>> range = this->syntacticIntRange(expr)) {
    if (range->second <= limit) {
      return true;
    }
  }
  auto *name = dyn_cast<NameExpr>(expr);
  const VarSymbol *var = name ? this->scopes->lookup(name->name).var() : nullptr;

  return var && var->type->isInt() && !scan.changes.count(name->name) &&
         this->state.getFlowState().getFacts().proves(Term::of(var), Term::zero(),
                                                      Facts::saturatingAdd(limit, 1));
}

bool LoopBounds::boundedBelow(const Expr *expr, int64_t limit, const LoopScan &scan) const {
  if (std::optional<std::pair<int64_t, int64_t>> range = this->syntacticIntRange(expr)) {
    if (range->first >= limit) {
      return true;
    }
  }
  auto *name = dyn_cast<NameExpr>(expr);
  const VarSymbol *var = name ? this->scopes->lookup(name->name).var() : nullptr;

  return var && var->type->isInt() && !scan.changes.count(name->name) &&
         this->state.getFlowState().getFacts().proves(Term::zero(), Term::of(var),
                                                      Facts::saturatingSub(1, limit));
}

std::optional<int64_t> LoopBounds::condBoundsAbove(const Expr *cond, const VarSymbol *var,
                                                   const LoopScan &scan, int64_t step) const {
  for (const Expr *part : ExprParts::andParts(cond)) {
    std::optional<int64_t> k = this->comparisonBoundsAbove(part, var, scan, step);
    if (k.has_value()) {
      return k;
    }
  }

  return std::nullopt;
}

std::optional<int64_t> LoopBounds::comparisonBoundsAbove(const Expr *part, const VarSymbol *var,
                                                         const LoopScan &scan, int64_t step) const {
  auto *bin = llvm::dyn_cast_or_null<BinaryExpr>(part);
  if (!bin) {
    return std::nullopt;
  }
  auto lookupVar = [&](llvm::StringRef name) -> const VarSymbol * {
    return this->scopes->lookup(name).var();
  };
  const Expr *small = nullptr;
  const Expr *large = nullptr;
  bool strict = false;
  switch (bin->op) {
  case BinaryOp::Lt:
    small = bin->lhs;
    large = bin->rhs;
    strict = true;
    break;
  case BinaryOp::Gt:
    small = bin->rhs;
    large = bin->lhs;
    strict = true;
    break;
  case BinaryOp::Le:
    small = bin->lhs;
    large = bin->rhs;
    break;
  case BinaryOp::Ge:
    small = bin->rhs;
    large = bin->lhs;
    break;
  default:
    return std::nullopt;
  }
  std::optional<int64_t> k = LoopBounds::varPlus(small, var, lookupVar);
  if (k && this->boundedAbove(large, strict ? kIntMax - (step - 1) : kIntMax - step, scan)) {
    return k;
  }

  return std::nullopt;
}

bool LoopBounds::condBoundsBelow(const Expr *cond, const VarSymbol *var, const LoopScan &scan,
                                 int64_t step) const {
  for (const Expr *part : ExprParts::andParts(cond)) {
    if (this->comparisonBoundsBelow(part, var, scan, step)) {
      return true;
    }
  }

  return false;
}

bool LoopBounds::comparisonBoundsBelow(const Expr *part, const VarSymbol *var, const LoopScan &scan,
                                       int64_t step) const {
  auto *bin = llvm::dyn_cast_or_null<BinaryExpr>(part);
  if (!bin) {
    return false;
  }
  auto isVar = [&](const Expr *e) {
    auto *name = dyn_cast<NameExpr>(e);

    return name && this->scopes->lookup(name->name).var() == var;
  };
  switch (bin->op) {
  case BinaryOp::Gt:
    return isVar(bin->lhs) && this->boundedBelow(bin->rhs, kIntMin + step - 1, scan);
  case BinaryOp::Lt:
    return isVar(bin->rhs) && this->boundedBelow(bin->lhs, kIntMin + step - 1, scan);
  case BinaryOp::Ge:
    return isVar(bin->lhs) && this->boundedBelow(bin->rhs, kIntMin + step, scan);
  case BinaryOp::Le:
    return isVar(bin->rhs) && this->boundedBelow(bin->lhs, kIntMin + step, scan);
  default:
    return false;
  }
}

Flow LoopBounds::loopHead(const LoopScan &scan, const Expr *cond, bool induction) const {
  Flow head = this->state.getFlowState().getFlow();
  head.unreachable = false;
  for (const auto &entry : scan.changes) {
    this->forgetChanged(head, scan, entry.first(), entry.second, cond, induction);
  }
  LoopBounds::forgetWrittenFields(head, scan);
  if (scan.callsFunctions) {
    this->flowChecker->forgetCallEffects(head);
  }
  LoopBounds::forgetArrayFacts(head, scan);

  return head;
}

void LoopBounds::forgetChanged(Flow &head, const LoopScan &scan, llvm::StringRef name,
                               const LoopScan::Change &change, const Expr *cond,
                               bool induction) const {
  const VarSymbol *var = this->scopes->lookup(name).var();
  KeptBounds kept = induction ? this->keptBounds(scan, change, var, cond) : KeptBounds{};
  head.facts.forgetIf(
      [&](const Facts::Fact &f) { return LoopBounds::isStaleFact(f, name, var, kept); });
  if (kept.invariant.has_value()) {
    head.facts.add(Term::of(var), Term::zero(), Facts::saturatingAdd(kIntMax - *kept.invariant, 1));
  }
  LoopBounds::forgetNamed(head, name);
}

LoopBounds::KeptBounds LoopBounds::keptBounds(const LoopScan &scan, const LoopScan::Change &change,
                                              const VarSymbol *var, const Expr *cond) const {
  KeptBounds kept;
  bool counter = var != nullptr && var->type->isInt() && change.count == 1 && !change.nested;
  if (!counter) {
    return kept;
  }
  std::optional<int64_t> k =
      change.delta > 0 ? this->condBoundsAbove(cond, var, scan, change.delta) : std::nullopt;
  if (k.has_value() && *k == 0) {
    kept.lower = true;
  } else if (k.has_value() &&
             this->state.getFlowState().getFacts().proves(Term::of(var), Term::zero(),
                                                          Facts::saturatingAdd(kIntMax - *k, 1))) {
    kept.lower = true;
    kept.invariant = *k;
  }
  kept.upper = change.delta < 0 && this->condBoundsBelow(cond, var, scan, -change.delta);

  return kept;
}

bool LoopBounds::isStaleFact(const Facts::Fact &f, llvm::StringRef name, const VarSymbol *var,
                             const KeptBounds &kept) {
  bool lhsHit = LoopBounds::termNames(f.lhs, name);
  bool rhsHit = LoopBounds::termNames(f.rhs, name);
  if (!lhsHit && !rhsHit) {
    return false;
  }
  if (kept.lower && !lhsHit && f.rhs == Term::of(var)) {
    return false;
  }
  if (kept.upper && !rhsHit && f.lhs == Term::of(var)) {
    return false;
  }

  return true;
}

bool LoopBounds::termNames(const Term &t, llvm::StringRef name) {
  return (t.var != nullptr && t.var->name == name) || (t.var2 != nullptr && t.var2->name == name);
}

void LoopBounds::forgetNamed(Flow &head, llvm::StringRef name) {
  for (const VarSymbol *defined : llvm::to_vector(head.defined)) {
    if (defined->name == name) {
      head.defined.erase(defined);
    }
  }
  llvm::erase_if(head.narrowed, [&](const auto &narrowed) { return narrowed.first->name == name; });
  for (auto *results : {&head.okResults, &head.errResults}) {
    for (const VarSymbol *result : llvm::to_vector(*results)) {
      if (result->name == name) {
        results->erase(result);
      }
    }
  }
  llvm::erase_if(head.definedFields, [&](const FieldPath &path) { return path.var->name == name; });
  llvm::erase_if(head.narrowedFields,
                 [&](const auto &entry) { return entry.first.var->name == name; });
}

void LoopBounds::forgetWrittenFields(Flow &head, const LoopScan &scan) {
  llvm::erase_if(head.definedFields,
                 [&](const FieldPath &path) { return LoopBounds::isWrittenPath(path, scan); });
  llvm::erase_if(head.narrowedFields,
                 [&](const auto &entry) { return LoopBounds::isWrittenPath(entry.first, scan); });
}

bool LoopBounds::isWrittenPath(const FieldPath &path, const LoopScan &scan) {
  for (llvm::StringRef field : path.names) {
    if (scan.fieldWrites.contains(field)) {
      return true;
    }
  }

  return false;
}

void LoopBounds::forgetArrayFacts(Flow &head, const LoopScan &scan) {
  head.facts.forgetIf([&](const Facts::Fact &f) {
    return (scan.growsArrays && LoopBounds::isArrayPart(f.lhs)) ||
           (scan.shrinksArrays && LoopBounds::isArrayPart(f.rhs));
  });
}

bool LoopBounds::isArrayPart(const Term &t) {
  return (t.length && LoopBounds::isArrayLength(t.first())) ||
         (t.length2 && LoopBounds::isArrayLength(t.second()));
}

}
