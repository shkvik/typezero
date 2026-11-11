#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/program/library/array/array_method_table/array_method_table.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/semantic/flow/field_facts/field_facts.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"
#include "compiler/semantic/flow/union_narrowing/union_narrowing.h"
#include "compiler/semantic/library/bytes/byte_proofs/byte_proofs.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include <limits>

using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

constexpr int64_t kIntMin = std::numeric_limits<int64_t>::min();

constexpr int64_t kIntMax = std::numeric_limits<int64_t>::max();

}

FlowChecker::FlowChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->byteProofs = &all.byteProofs;
  this->indexProofs = &all.indexProofs;
}

void FlowChecker::addLess(Facts &facts, const Linear &x, const Linear &y, bool orEqual) {
  facts.add(x.term, y.term, y.offset - x.offset + (orEqual ? 1 : 0));
}

bool FlowChecker::isArrayLength(const Term &t) {
  return t.length && t.var->type->nonOptional()->isArray();
}

bool FlowChecker::changedByCalls(const VarSymbol *var) const {
  if (var->isConst) {
    return false;
  }
  if (var->isGlobal()) {
    return var->usedInFunction();
  }
  if (var->captured()) {
    return true;
  }

  return this->state.getFunctionStack().isAssignedInGroup(var->name);
}

Flow FlowChecker::join(const Flow &a, const Flow &b) {
  if (a.unreachable) {
    return b;
  }
  if (b.unreachable) {
    return a;
  }
  Flow result;
  result.facts = Facts::meet(a.facts, b.facts);
  FlowChecker::joinDefined(a, b, result);
  for (const NarrowedVar &narrowed : a.narrowed) {
    std::optional<NarrowedVar> both = FlowChecker::narrowedOnBoth(narrowed, b);
    if (both.has_value()) {
      result.narrowed.push_back(*both);
    }
  }
  FlowChecker::widenWithUndefined(a, b, result);
  FlowChecker::widenWithUndefined(b, a, result);
  FlowChecker::joinMarks(a, b, result);
  result.beforeSuper = a.beforeSuper || b.beforeSuper;

  return result;
}

void FlowChecker::joinDefined(const Flow &a, const Flow &b, Flow &result) {
  for (const VarSymbol *var : a.defined) {
    if (b.defined.contains(var)) {
      result.defined.insert(var);
    }
  }
  for (const FieldPath &path : a.definedFields) {
    if (llvm::is_contained(b.definedFields, path)) {
      result.definedFields.push_back(path);
    }
  }
  for (const auto &narrowed : a.narrowedFields) {
    if (llvm::is_contained(b.narrowedFields, narrowed)) {
      result.narrowedFields.push_back(narrowed);
    }
  }
}

const FlowChecker::NarrowedVar *FlowChecker::narrowedIn(const Flow &flow, const VarSymbol *var) {
  for (const NarrowedVar &narrowed : flow.narrowed) {
    if (narrowed.first == var) {
      return &narrowed;
    }
  }

  return nullptr;
}

std::optional<FlowChecker::NarrowedVar> FlowChecker::narrowedOnBoth(const NarrowedVar &narrowed,
                                                                    const Flow &b) {
  if (llvm::is_contained(b.narrowed, narrowed)) {
    return narrowed;
  }
  auto [var, type] = narrowed;
  const NarrowedVar *other = FlowChecker::narrowedIn(b, var);
  bool generic = other == nullptr || var->type->nonOptional()->isParam() || type->isParam() ||
                 other->second->isParam();
  if (generic) {
    return std::nullopt;
  }
  const Type *either = Type::unionOf({type, other->second});
  if (either->isError() || !UnionSlots::unionProblem(either).empty() || either == var->type) {
    return std::nullopt;
  }
  if (var->type->isOptional() && either == var->type->inner()) {
    return std::nullopt;
  }

  return NarrowedVar{var, either};
}

void FlowChecker::widenWithUndefined(const Flow &empty, const Flow &other, Flow &result) {
  for (const VarSymbol *var : empty.undefinedVars) {
    const NarrowedVar *narrowed = FlowChecker::narrowedIn(other, var);
    bool skipped = narrowed == nullptr || var->type->nonOptional()->isParam() ||
                   var->type->nonOptional()->isJson() || narrowed->second->isParam() ||
                   FlowChecker::narrowedIn(result, var) != nullptr;
    if (skipped) {
      continue;
    }
    const Type *either = Type::unionOf({narrowed->second, Type::undefinedTy()});
    if (either->isError() || !UnionSlots::unionProblem(either).empty() || either == var->type) {
      continue;
    }
    result.narrowed.push_back({var, either});
  }
}

void FlowChecker::joinMarks(const Flow &a, const Flow &b, Flow &result) {
  for (const VarSymbol *var : a.undefinedVars) {
    if (b.undefinedVars.contains(var)) {
      result.undefinedVars.insert(var);
    }
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
  for (const EmitterKey &key : a.errorListened) {
    if (llvm::is_contained(b.errorListened, key)) {
      result.errorListened.push_back(key);
    }
  }
  result.errorPending = a.errorPending;
  for (const auto &pending : b.errorPending) {
    if (!FlowChecker::hasPending(result.errorPending, pending.first)) {
      result.errorPending.push_back(pending);
    }
  }
}

bool FlowChecker::hasPending(llvm::ArrayRef<std::pair<EmitterKey, llvm::SMLoc>> pending,
                             const EmitterKey &key) {
  for (const auto &p : pending) {
    if (p.first == key) {
      return true;
    }
  }

  return false;
}

void FlowChecker::setDefined(const VarSymbol *var, bool defined) {
  if (defined) {
    this->state.getFlowState().markDefined(var);
  } else {
    this->state.getFlowState().forgetDefined(var);
  }
}

void FlowChecker::assigned(const VarSymbol *var, const Expr *value) {
  const Type *type = var->type;
  if (type->isError()) {
    return;
  }
  this->resetAssignedFlow(var, value);
  if (type->isOptional()) {
    this->setDefined(var, !value->type->isOptional() && !value->type->isUndefined());
  }
  if (type->isResult()) {
    return this->rememberResultState(var, value->type);
  }
  if (type->isInteger() && !type->isWrapping()) {
    return this->assignedInteger(var, value);
  }
  this->state.getFlowState().getFacts().forget(var);

  return this->assignedLength(var, value);
}

void FlowChecker::resetAssignedFlow(const VarSymbol *var, const Expr *value) {
  const Type *type = var->type;
  Flow flow = this->state.getFlowState().takeFlow();
  FieldFacts::forgetFieldsOf(flow, var);
  flow.undefinedVars.erase(var);
  std::vector<NarrowedVar> kept;
  for (const NarrowedVar &narrowed : flow.narrowed) {
    if (narrowed.first != var) {
      kept.push_back(narrowed);
    }
  }
  flow.narrowed = std::move(kept);
  const Type *base = type->nonOptional();
  bool narrows = base->isUnion() && value->type != nullptr && !value->type->isError() &&
                 value->type != type && TypeRelations::isImplicitlyConvertible(value->type, base);
  if (narrows) {
    UnionNarrowing::narrowVariable(flow, var, value->type);
  }
  this->state.getFlowState().replaceFlow(std::move(flow));
}

void FlowChecker::rememberResultState(const VarSymbol *var, const Type *given) {
  this->state.getFlowState().forgetResultState(var);
  if (!given->isResult()) {
    return;
  }
  if (given->errType()->isNever()) {
    this->state.getFlowState().markOkResult(var);
  } else if (given->okType()->isNever()) {
    this->state.getFlowState().markErrResult(var);
  }
}

void FlowChecker::assignedInteger(const VarSymbol *var, const Expr *value) {
  std::optional<Linear> v = this->indexProofs->linear(value);
  if (v.has_value() && v->term == Term::of(var)) {
    return this->incremented(var, v->offset);
  }
  IntegerRange range = v.has_value() ? IntegerRange{} : this->byteProofs->valueRange(value);
  Facts &facts = this->state.getFlowState().getFacts();
  facts.forget(var);
  if (v.has_value()) {
    facts.addEqual(Term::of(var), v->term, v->offset);
  }
  if (range.lo.has_value()) {
    facts.add(Term::zero(), Term::of(var), Facts::saturatingSub(1, *range.lo));
  }
  if (range.hi.has_value()) {
    facts.add(Term::of(var), Term::zero(), Facts::saturatingAdd(*range.hi, 1));
  }

  return this->addMinMaxBounds(var, value);
}

void FlowChecker::addMinMaxBounds(const VarSymbol *var, const Expr *value) {
  auto *call = dyn_cast<CallExpr>(value);
  bool minMax = call != nullptr &&
                (call->builtin() == Builtin::MathMin || call->builtin() == Builtin::MathMax) &&
                !call->hasSpread();
  if (!minMax) {
    return;
  }
  Linear self{Term::of(var), 0};
  for (const Expr *arg : call->args) {
    std::optional<Linear> bound = this->indexProofs->linear(arg);
    if (!bound.has_value()) {
      continue;
    }
    if (call->builtin() == Builtin::MathMin) {
      FlowChecker::addLess(this->state.getFlowState().getFacts(), self, *bound, true);
    } else {
      FlowChecker::addLess(this->state.getFlowState().getFacts(), *bound, self, true);
    }
  }
}

void FlowChecker::assignedLength(const VarSymbol *var, const Expr *value) {
  const Type *base = var->type->nonOptional();
  bool sized = base->isString() || base->isArray() || base->isByteView() ||
               base->bytes() == Type::Bytes::ArrayBuffer;
  if (!sized || value->type->isOptional() || value->type->isUndefined()) {
    return;
  }
  Facts &facts = this->state.getFlowState().getFacts();
  std::optional<Linear> length = this->indexProofs->knownLength(value);
  if (length.has_value()) {
    if (length->term != Term::lengthOf(var)) {
      facts.addEqual(Term::lengthOf(var), length->term, length->offset);
    }
    return;
  }
  IntegerRange range = this->indexProofs->lengthRange(value);
  if (range.lo.has_value()) {
    facts.add(Term::zero(), Term::lengthOf(var), Facts::saturatingSub(1, *range.lo));
  }
  if (range.hi.has_value()) {
    facts.add(Term::lengthOf(var), Term::zero(), Facts::saturatingAdd(*range.hi, 1));
  }
}

void FlowChecker::incremented(const VarSymbol *var, int64_t delta) {
  if (!var->type->isInt()) {
    return;
  }
  Term t = Term::of(var);
  bool safe =
      delta == 0 || (delta > 0 ? this->state.getFlowState().getFacts().proves(t, Term::zero(),
                                                                              kIntMax - (delta - 1))
                               : this->state.getFlowState().getFacts().proves(
                                     Term::zero(), t, Facts::saturatingSub(delta + 1, kIntMin)));
  if (safe) {
    this->state.getFlowState().getFacts().shift(t, delta);
  } else {
    this->state.getFlowState().getFacts().forget(var);
  }
}

bool FlowChecker::staysLocal(const FunctionDecl *fn, llvm::StringRef name) {
  if (fn == nullptr || fn->body == nullptr) {
    return false;
  }
  PlacedNodes nodes = PlacedNodes::collect({fn->body}, {});
  size_t uses = FlowChecker::countUses(nodes, name);
  size_t localUses = FlowChecker::countLocalUses(nodes, name);

  return localUses == uses;
}

size_t FlowChecker::countUses(const PlacedNodes &nodes, llvm::StringRef name) {
  size_t uses = 0;
  for (const PlacedExpr &placed : nodes.getExprs()) {
    if (FlowChecker::isName(placed.expr, name)) {
      uses += 1;
    }
  }

  return uses;
}

size_t FlowChecker::countLocalUses(const PlacedNodes &nodes, llvm::StringRef name) {
  size_t uses = 0;
  for (const PlacedExpr &placed : nodes.getExprs()) {
    if (placed.place.isInsideFunction()) {
      continue;
    }
    if (FlowChecker::readsElement(placed.expr, name)) {
      uses += 1;
    }
    if (FlowChecker::callsArrayMethod(placed.expr, name)) {
      uses += 1;
    }
    uses += FlowChecker::countSpreads(placed.expr, name);
  }
  for (const PlacedStmt &placed : nodes.getStmts()) {
    if (placed.place.isInsideFunction()) {
      continue;
    }
    if (FlowChecker::iteratesOver(placed.stmt, name)) {
      uses += 1;
    }
    if (FlowChecker::returnsName(placed.stmt, name)) {
      uses += 1;
    }
  }

  return uses;
}

bool FlowChecker::isName(const Expr *expr, llvm::StringRef name) {
  auto *named = dyn_cast_or_null<NameExpr>(expr);

  return named != nullptr && named->name == name;
}

bool FlowChecker::readsElement(const Expr *expr, llvm::StringRef name) {
  if (auto *index = dyn_cast<IndexExpr>(expr)) {
    return FlowChecker::isName(index->object, name);
  }
  if (auto *member = dyn_cast<MemberExpr>(expr)) {
    bool length = BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH);
    return length && FlowChecker::isName(member->object, name);
  }

  return false;
}

size_t FlowChecker::countSpreads(const Expr *expr, llvm::StringRef name) {
  size_t spreads = 0;
  if (auto *list = dyn_cast<ArrayLitExpr>(expr)) {
    for (const ArrayLitExpr::Element &element : list->elements) {
      if (element.spread && FlowChecker::isName(element.expr, name)) {
        spreads += 1;
      }
    }
  }
  if (auto *call = dyn_cast<CallExpr>(expr)) {
    for (size_t i = 0; i < call->args.size(); ++i) {
      if (call->spreads(i) && FlowChecker::isName(call->args[i], name)) {
        spreads += 1;
      }
    }
  }

  return spreads;
}

bool FlowChecker::callsArrayMethod(const Expr *expr, llvm::StringRef name) {
  auto *call = dyn_cast<CallExpr>(expr);
  if (call == nullptr) {
    return false;
  }
  auto *callee = dyn_cast<MemberExpr>(call->callee);
  if (callee == nullptr || !FlowChecker::isName(callee->object, name)) {
    return false;
  }
  const ArrayMethodInfo *method = ArrayMethodTable::find(callee->name);
  if (method == nullptr) {
    return false;
  }
  const CallbackArrayMethod *callback = method->form.as<CallbackArrayMethod>();
  bool sorts = method->form.is<SortingArrayMethod>();
  if (callback == nullptr && !sorts) {
    return true;
  }
  bool reduces = callback != nullptr && callback->callback == ArrayCallback::REDUCE;
  size_t arrayParam = reduces ? 3 : 2;

  return FlowChecker::callbacksSkipArray(call, arrayParam);
}

bool FlowChecker::callbacksSkipArray(const CallExpr *call, size_t arrayParam) {
  for (size_t i = 0; i < call->args.size(); ++i) {
    auto *callback = dyn_cast<FunctionExpr>(call->args[i]);
    if (callback == nullptr && i == 0) {
      return false;
    }
    if (callback != nullptr && callback->fn->params.size() > arrayParam) {
      return false;
    }
  }

  return true;
}

bool FlowChecker::iteratesOver(const Stmt *stmt, llvm::StringRef name) {
  auto *loop = dyn_cast<ForOfStmt>(stmt);

  return loop != nullptr && FlowChecker::isName(loop->iterable, name);
}

bool FlowChecker::returnsName(const Stmt *stmt, llvm::StringRef name) {
  auto *result = dyn_cast<ReturnStmt>(stmt);
  if (result == nullptr) {
    return false;
  }
  const Expr *value = FlowChecker::unwrapResult(result->value);

  return FlowChecker::isName(value, name);
}

const Expr *FlowChecker::unwrapResult(const Expr *value) {
  auto *wrap = dyn_cast_or_null<CallExpr>(value);
  if (wrap == nullptr || wrap->args.size() != 1) {
    return value;
  }
  auto *maker = dyn_cast<NameExpr>(wrap->callee);
  if (maker == nullptr) {
    return value;
  }
  std::optional<GlobalName> global = GlobalNameTable::find(maker->name);
  bool wraps = global == GlobalName::OK || global == GlobalName::ERR;

  return wraps ? wrap->args[0] : value;
}

void FlowChecker::forgetCallEffects(Flow &flow) const {
  auto part = [&](const Term &t) {
    return t.var &&
           ((FlowChecker::isArrayLength(t) && !t.var->unshared()) || this->changedByCalls(t.var));
  };
  auto changed = [&](const Term &t) { return part(t.first()) || part(t.second()); };
  flow.facts.forgetIf([&](const Facts::Fact &f) { return changed(f.lhs) || changed(f.rhs); });
  for (const VarSymbol *var : llvm::to_vector(flow.defined)) {
    if (this->changedByCalls(var)) {
      flow.defined.erase(var);
    }
  }
  for (const VarSymbol *var : llvm::to_vector(flow.undefinedVars)) {
    if (this->changedByCalls(var)) {
      flow.undefinedVars.erase(var);
    }
  }
  llvm::erase_if(flow.narrowed,
                 [&](const auto &narrowed) { return this->changedByCalls(narrowed.first); });
  for (auto *results : {&flow.okResults, &flow.errResults}) {
    for (const VarSymbol *var : llvm::to_vector(*results)) {
      if (this->changedByCalls(var)) {
        results->erase(var);
      }
    }
  }
  auto fieldChanged = [&](const FieldPath &path) {
    return this->changedByCalls(path.var) || llvm::any_of(path.names, [&](llvm::StringRef field) {
             return this->state.getFlowState().isFieldAssigned(field);
           });
  };
  llvm::erase_if(flow.definedFields, fieldChanged);
  llvm::erase_if(flow.narrowedFields, [&](const auto &entry) { return fieldChanged(entry.first); });
}

void FlowChecker::calledFunction() {
  Flow flow = this->state.getFlowState().takeFlow();
  this->forgetCallEffects(flow);
  this->state.getFlowState().replaceFlow(std::move(flow));
}

void FlowChecker::arrayResized(const Type *type, bool grew, bool shrank, const VarSymbol *except) {
  type = type->nonOptional();
  auto part = [&](const Term &t) {
    return t.length && t.var != except && t.var->type->nonOptional() == type;
  };
  auto affected = [&](const Term &t) { return part(t.first()) || part(t.second()); };
  this->state.getFlowState().getFacts().forgetIf([&](const Facts::Fact &f) {
    return (grew && affected(f.lhs)) || (shrank && affected(f.rhs));
  });
}

}
