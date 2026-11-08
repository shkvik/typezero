#include "compiler/semantic/flow/loop_bounds/loop_bounds.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
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

LoopBounds::LoopBounds(SemanticState &owner)
    : state(owner) {}

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
    const VarSymbol *var = Scopes(this->state).lookup(name->name).var();
    if (var && var->type->isInt()) {
      return std::pair{kIntMin, kIntMax};
    }
    return std::nullopt;
  }
  if (auto *member = dyn_cast<MemberExpr>(expr)) {
    auto *name = dyn_cast<NameExpr>(member->object);
    const VarSymbol *var = name ? Scopes(this->state).lookup(name->name).var() : nullptr;
    if (member->name == "byteLength" && var && var->type->isBytes()) {
      return std::pair{int64_t(0), Type::kMaxLength};
    }
    if (member->name != "length") {
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
  if (auto *bin = dyn_cast<BinaryExpr>(expr)) {
    auto *lit = dyn_cast<IntLitExpr>(bin->rhs);
    if (bin->op != BinaryOp::Sub || !isa<MemberExpr>(bin->lhs) || !lit ||
        lit->value > kMaxConstant) {
      return std::nullopt;
    }
    std::optional<std::pair<int64_t, int64_t>> length = this->syntacticIntRange(bin->lhs);
    if (!length) {
      return std::nullopt;
    }
    auto k = static_cast<int64_t>(lit->value);
    return std::pair{length->first - k, length->second - k};
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
  const VarSymbol *var = name ? Scopes(this->state).lookup(name->name).var() : nullptr;

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
  const VarSymbol *var = name ? Scopes(this->state).lookup(name->name).var() : nullptr;

  return var && var->type->isInt() && !scan.changes.count(name->name) &&
         this->state.getFlowState().getFacts().proves(Term::zero(), Term::of(var),
                                                      Facts::saturatingSub(1, limit));
}

std::optional<int64_t> LoopBounds::condBoundsAbove(const Expr *cond, const VarSymbol *var,
                                                   const LoopScan &scan, int64_t step) const {
  auto *bin = llvm::dyn_cast_or_null<BinaryExpr>(cond);
  if (!bin) {
    return std::nullopt;
  }
  if (bin->op == BinaryOp::LogAnd) {
    if (std::optional<int64_t> k = this->condBoundsAbove(bin->lhs, var, scan, step)) {
      return k;
    }
    return this->condBoundsAbove(bin->rhs, var, scan, step);
  }
  auto lookupVar = [&](llvm::StringRef name) -> const VarSymbol * {
    return Scopes(this->state).lookup(name).var();
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
  auto *bin = llvm::dyn_cast_or_null<BinaryExpr>(cond);
  if (!bin) {
    return false;
  }
  if (bin->op == BinaryOp::LogAnd) {
    return this->condBoundsBelow(bin->lhs, var, scan, step) ||
           this->condBoundsBelow(bin->rhs, var, scan, step);
  }
  auto isVar = [&](const Expr *e) {
    auto *name = dyn_cast<NameExpr>(e);

    return name && Scopes(this->state).lookup(name->name).var() == var;
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
    llvm::StringRef name = entry.first();
    const LoopScan::Change &change = entry.second;
    const VarSymbol *var = Scopes(this->state).lookup(name).var();

    bool keepLower = false;
    bool keepUpper = false;
    std::optional<int64_t> invariant;
    if (induction && var && var->type->isInt() && change.count == 1 && !change.nested) {
      if (change.delta > 0) {
        if (std::optional<int64_t> k = this->condBoundsAbove(cond, var, scan, change.delta)) {
          if (*k == 0) {
            keepLower = true;
          } else if (this->state.getFlowState().getFacts().proves(
                         Term::of(var), Term::zero(), Facts::saturatingAdd(kIntMax - *k, 1))) {
            keepLower = true;
            invariant = *k;
          }
        }
      }
      keepUpper = change.delta < 0 && this->condBoundsBelow(cond, var, scan, -change.delta);
    }
    head.facts.forgetIf([&](const Facts::Fact &f) {
      auto hits = [&](const Term &t) {
        return (t.var && t.var->name == name) || (t.var2 && t.var2->name == name);
      };
      bool lhsHit = hits(f.lhs);
      bool rhsHit = hits(f.rhs);
      if (!lhsHit && !rhsHit) {
        return false;
      }
      if (keepLower && !lhsHit && f.rhs == Term::of(var)) {
        return false;
      }
      if (keepUpper && !rhsHit && f.lhs == Term::of(var)) {
        return false;
      }

      return true;
    });
    if (invariant) {
      head.facts.add(Term::of(var), Term::zero(), Facts::saturatingAdd(kIntMax - *invariant, 1));
    }
    for (const VarSymbol *defined : llvm::to_vector(head.defined)) {
      if (defined->name == name) {
        head.defined.erase(defined);
      }
    }
    llvm::erase_if(head.narrowed,
                   [&](const auto &narrowed) { return narrowed.first->name == name; });
    for (auto *results : {&head.okResults, &head.errResults}) {
      for (const VarSymbol *result : llvm::to_vector(*results)) {
        if (result->name == name) {
          results->erase(result);
        }
      }
    }
    llvm::erase_if(head.definedFields,
                   [&](const FieldPath &path) { return path.var->name == name; });
    llvm::erase_if(head.narrowedFields,
                   [&](const auto &entry) { return entry.first.var->name == name; });
  }
  auto written = [&](const FieldPath &path) {
    return llvm::any_of(path.names,
                        [&](llvm::StringRef field) { return scan.fieldWrites.contains(field); });
  };
  llvm::erase_if(head.definedFields, written);
  llvm::erase_if(head.narrowedFields, [&](const auto &entry) { return written(entry.first); });
  if (scan.callsFunctions) {
    FlowChecker(this->state).forgetCallEffects(head);
  }
  auto arrayPart = [](const Term &t) {
    return (t.length && LoopBounds::isArrayLength(t.first())) ||
           (t.length2 && LoopBounds::isArrayLength(t.second()));
  };
  head.facts.forgetIf([&](const Facts::Fact &f) {
    return (scan.growsArrays && arrayPart(f.lhs)) || (scan.shrinksArrays && arrayPart(f.rhs));
  });

  return head;
}

}
