#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/flow/field_facts/field_facts.h"
#include "compiler/check/flow/index_proofs/index_proofs.h"
#include "compiler/check/flow/union_narrowing/union_narrowing.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/library/bytes/byte_proofs/byte_proofs.h"
#include "llvm/ADT/STLExtras.h"
#include <limits>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

constexpr int64_t kIntMin = std::numeric_limits<int64_t>::min();

constexpr int64_t kIntMax = std::numeric_limits<int64_t>::max();

void addLess(Facts &facts, const Linear &x, const Linear &y, bool orEqual) {
  facts.add(x.term, y.term, y.offset - x.offset + (orEqual ? 1 : 0));
}

bool isArrayLength(const Term &t) { return t.length && t.var->type->nonOptional()->isArray(); }

}

FlowChecker::FlowChecker(Checker &checker) : checker_(checker) {}

bool FlowChecker::changedByCalls(const VarSymbol *var) const {
  if (var->isConst) {
    return false;
  }
  if (var->isGlobal) {
    return var->usedInFunction;
  }
  if (var->captured) {
    return true;
  }

  return llvm::any_of(checker_.groups_, [&](const NestedGroup *g) {
    return llvm::is_contained(g->assignedNames, var->name);
  });
}

Checker::Flow FlowChecker::join(const Checker::Flow &a, const Checker::Flow &b) {
  if (a.unreachable) {
    return b;
  }
  if (b.unreachable) {
    return a;
  }
  Checker::Flow result;
  result.facts = Facts::meet(a.facts, b.facts);
  for (const VarSymbol *var : a.defined) {
    if (b.defined.contains(var)) {
      result.defined.insert(var);
    }
  }
  for (const Checker::FieldPath &path : a.definedFields) {
    if (llvm::is_contained(b.definedFields, path)) {
      result.definedFields.push_back(path);
    }
  }
  for (const auto &narrowed : a.narrowedFields) {
    if (llvm::is_contained(b.narrowedFields, narrowed)) {
      result.narrowedFields.push_back(narrowed);
    }
  }
  for (const auto &narrowed : a.narrowed) {
    if (llvm::is_contained(b.narrowed, narrowed)) {
      result.narrowed.push_back(narrowed);
      continue;
    }
    auto [var, type] = narrowed;
    auto other = llvm::find_if(b.narrowed, [&](const auto &n) { return n.first == var; });
    if (other == b.narrowed.end() || var->type->nonOptional()->isParam() || type->isParam() ||
        other->second->isParam()) {
      continue;
    }
    const Type *either = Type::unionOf({type, other->second});
    if (either->isError() || !unionProblem(either).empty() || either == var->type) {
      continue;
    }
    if (var->type->isOptional() && either == var->type->inner()) {
      continue;
    }
    result.narrowed.push_back({var, either});
  }
  for (const VarSymbol *var : a.okResults) {
    if (b.okResults.contains(var)) {
      result.okResults.insert(var);
    }
  }
  for (const VarSymbol *var : a.errResults) {
    if (b.errResults.contains(var)) {
      result.errResults.insert(var);
    }
  }
  for (llvm::StringRef field : a.setFields) {
    if (llvm::is_contained(b.setFields, field)) {
      result.setFields.push_back(field);
    }
  }
  for (const Checker::EmitterKey &key : a.errorListened) {
    if (llvm::is_contained(b.errorListened, key)) {
      result.errorListened.push_back(key);
    }
  }
  result.errorPending = a.errorPending;
  for (const auto &pending : b.errorPending) {
    if (llvm::none_of(result.errorPending,
                      [&](const auto &p) { return p.first == pending.first; })) {
      result.errorPending.push_back(pending);
    }
  }
  result.beforeSuper = a.beforeSuper || b.beforeSuper;

  return result;
}

void FlowChecker::setDefined(const VarSymbol *var, bool defined) {
  if (defined) {
    checker_.flow_.defined.insert(var);
  } else {
    checker_.flow_.defined.erase(var);
  }
}

void FlowChecker::assigned(const VarSymbol *var, const Expr *value) {
  const Type *type = var->type;
  if (type->isError()) {
    return;
  }
  FieldFacts::forgetFieldsOf(checker_.flow_, var);
  llvm::erase_if(checker_.flow_.narrowed,
                 [&](const auto &narrowed) { return narrowed.first == var; });
  if (const Type *base = type->nonOptional();
      base->isUnion() && value->type && !value->type->isError() && value->type != type &&
      TypeRelations::isImplicitlyConvertible(value->type, base)) {
    UnionNarrowing::narrowVariable(checker_.flow_, var, value->type);
  }
  if (type->isOptional()) {
    setDefined(var, !value->type->isOptional() && !value->type->isUndefined());
  }
  if (type->isResult()) {
    const Type *given = value->type;
    checker_.flow_.okResults.erase(var);
    checker_.flow_.errResults.erase(var);
    if (given->isResult() && given->errType()->isNever()) {
      checker_.flow_.okResults.insert(var);
    } else if (given->isResult() && given->okType()->isNever()) {
      checker_.flow_.errResults.insert(var);
    }
    return;
  }

  if (type->isInt()) {
    std::optional<Linear> v = checker_.indexProofs().linear(value);
    if (v && v->term == Term::of(var)) {
      incremented(var, v->offset);
      return;
    }
    ValueRange range = v ? ValueRange{} : checker_.byteProofs().valueRange(value);
    checker_.flow_.facts.forget(var);
    if (v) {
      checker_.flow_.facts.addEqual(Term::of(var), v->term, v->offset);
    }
    if (range.lo) {
      checker_.flow_.facts.add(Term::zero(), Term::of(var), saturatingSub(1, *range.lo));
    }
    if (range.hi) {
      checker_.flow_.facts.add(Term::of(var), Term::zero(), saturatingAdd(*range.hi, 1));
    }
    if (auto *call = dyn_cast<CallExpr>(value);
        call && (call->builtin == Builtin::MathMin || call->builtin == Builtin::MathMax) &&
        !call->hasSpread()) {
      for (const Expr *arg : call->args) {
        if (std::optional<Linear> bound = checker_.indexProofs().linear(arg)) {
          if (call->builtin == Builtin::MathMin) {
            addLess(checker_.flow_.facts, Linear{Term::of(var), 0}, *bound, true);
          } else {
            addLess(checker_.flow_.facts, *bound, Linear{Term::of(var), 0}, true);
          }
        }
      }
    }
    return;
  }

  checker_.flow_.facts.forget(var);
  const Type *base = type->nonOptional();
  if (!base->isString() && !base->isArray() && !base->isByteView() &&
      base->bytes() != Type::Bytes::ArrayBuffer) {
    return;
  }
  if (value->type->isOptional() || value->type->isUndefined()) {
    return;
  }
  if (std::optional<Linear> length = checker_.indexProofs().knownLength(value)) {
    if (length->term != Term::lengthOf(var)) {
      checker_.flow_.facts.addEqual(Term::lengthOf(var), length->term, length->offset);
    }
    return;
  }
  ValueRange range = checker_.indexProofs().lengthRange(value);
  if (range.lo) {
    checker_.flow_.facts.add(Term::zero(), Term::lengthOf(var), saturatingSub(1, *range.lo));
  }
  if (range.hi) {
    checker_.flow_.facts.add(Term::lengthOf(var), Term::zero(), saturatingAdd(*range.hi, 1));
  }
}

void FlowChecker::incremented(const VarSymbol *var, int64_t delta) {
  if (!var->type->isInt()) {
    return;
  }
  Term t = Term::of(var);
  bool safe =
      delta == 0 ||
      (delta > 0 ? checker_.flow_.facts.proves(t, Term::zero(), kIntMax - (delta - 1))
                 : checker_.flow_.facts.proves(Term::zero(), t, saturatingSub(delta + 1, kIntMin)));
  if (safe) {
    checker_.flow_.facts.shift(t, delta);
  } else {
    checker_.flow_.facts.forget(var);
  }
}

void FlowChecker::forgetCallEffects(Checker::Flow &flow) const {
  auto part = [&](const Term &t) { return t.var && (isArrayLength(t) || changedByCalls(t.var)); };
  auto changed = [&](const Term &t) { return part(t.first()) || part(t.second()); };
  flow.facts.forgetIf([&](const Facts::Fact &f) { return changed(f.lhs) || changed(f.rhs); });
  for (const VarSymbol *var : llvm::to_vector(flow.defined)) {
    if (changedByCalls(var)) {
      flow.defined.erase(var);
    }
  }
  llvm::erase_if(flow.narrowed,
                 [&](const auto &narrowed) { return changedByCalls(narrowed.first); });
  for (auto *results : {&flow.okResults, &flow.errResults}) {
    for (const VarSymbol *var : llvm::to_vector(*results)) {
      if (changedByCalls(var)) {
        results->erase(var);
      }
    }
  }
  auto fieldChanged = [&](const Checker::FieldPath &path) {
    return changedByCalls(path.var) || llvm::any_of(path.names, [&](llvm::StringRef field) {
             return checker_.assignedFields_.contains(field);
           });
  };
  llvm::erase_if(flow.definedFields, fieldChanged);
  llvm::erase_if(flow.narrowedFields, [&](const auto &entry) { return fieldChanged(entry.first); });
}

void FlowChecker::calledFunction() { forgetCallEffects(checker_.flow_); }

void FlowChecker::arrayResized(const Type *type, bool grew, bool shrank, const VarSymbol *except) {
  type = type->nonOptional();
  auto part = [&](const Term &t) {
    return t.length && t.var != except && t.var->type->nonOptional() == type;
  };
  auto affected = [&](const Term &t) { return part(t.first()) || part(t.second()); };
  checker_.flow_.facts.forgetIf([&](const Facts::Fact &f) {
    return (grew && affected(f.lhs)) || (shrank && affected(f.rhs));
  });
}

}
