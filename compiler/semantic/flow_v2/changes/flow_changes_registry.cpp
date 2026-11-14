#include "compiler/semantic/flow_v2/changes/flow_changes_registry.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <functional>
#include <limits>

using llvm::dyn_cast;

namespace tz {

namespace {

constexpr int64_t kIntMin = std::numeric_limits<int64_t>::min();
constexpr int64_t kIntMax = std::numeric_limits<int64_t>::max();

}

FlowChangesRegistry::FlowChangesRegistry(SemanticState &owner)
    : state(owner) {}

void FlowChangesRegistry::addAtMost(Facts &facts, const Linear &smaller,
                                    const Linear &larger) const {
  facts.add(smaller.term, larger.term, larger.offset - smaller.offset + 1);
}

void FlowChangesRegistry::noteAssigned(const NoteAssigned &task) {
  const VarSymbol *variable = task.variable;
  const Expr *value = task.value;
  const Type *type = variable->type;
  if (type->isError()) {
    return;
  }
  this->resetAssignedFlow(variable, value);
  bool isValueDefined = !value->type->isOptional() && !value->type->isUndefined();
  if (type->isOptional() && isValueDefined) {
    this->state.walk.flow.markDefined(variable);
  }
  if (type->isOptional() && !isValueDefined) {
    this->state.walk.flow.forgetDefined(variable);
  }
  if (type->isResult()) {
    return this->rememberResultState(variable, value->type);
  }
  if (type->isInteger() && !type->isWrapping()) {
    return this->assignedInteger(variable, value);
  }
  this->state.walk.flow.getFacts().forget(variable);

  return this->assignedLength(variable, value);
}

void FlowChangesRegistry::resetAssignedFlow(const VarSymbol *variable, const Expr *value) {
  const Type *type = variable->type;
  auto flow = this->state.walk.flow.takeFlow();
  this->state.flow.changes.forgetFieldsOf(flow, variable);
  flow.undefinedVars.erase(variable);
  auto kept = std::vector<NarrowedVar>();
  for (const NarrowedVar &narrowed : flow.narrowed) {
    if (narrowed.first != variable) {
      kept.push_back(narrowed);
    }
  }
  flow.narrowed = std::move(kept);
  const Type *base = type->nonOptional();
  bool isNarrowing = base->isUnion() && value->type != nullptr && !value->type->isError() &&
                     value->type != type &&
                     this->state.types.relations.isImplicitlyConvertible(value->type, base);
  if (isNarrowing) {
    this->state.flow.narrowing.narrowVariable(flow, variable, value->type);
  }
  this->state.walk.flow.replaceFlow(std::move(flow));
}

void FlowChangesRegistry::rememberResultState(const VarSymbol *variable, const Type *given) {
  this->state.walk.flow.forgetResultState(variable);
  if (!given->isResult()) {
    return;
  }
  if (given->errType()->isNever()) {
    this->state.walk.flow.markOkResult(variable);
    return;
  }
  if (given->okType()->isNever()) {
    this->state.walk.flow.markErrResult(variable);
  }
}

void FlowChangesRegistry::assignedInteger(const VarSymbol *variable, const Expr *value) {
  auto linear = this->state.flow.measurements.linear(value);
  if (linear.has_value() && linear->term == Term::of(variable)) {
    return this->incremented(variable, linear->offset);
  }
  IntegerRange range =
      linear.has_value() ? IntegerRange{} : this->state.flow.measurements.valueRange(value);
  auto &facts = this->state.walk.flow.getFacts();
  facts.forget(variable);
  if (linear.has_value()) {
    facts.addEqual(Term::of(variable), linear->term, linear->offset);
  }
  if (range.lo.has_value()) {
    facts.add(Term::zero(), Term::of(variable), Facts::saturatingSub(1, *range.lo));
  }
  if (range.hi.has_value()) {
    facts.add(Term::of(variable), Term::zero(), Facts::saturatingAdd(*range.hi, 1));
  }

  return this->addMinMaxBounds(variable, value);
}

void FlowChangesRegistry::addMinMaxBounds(const VarSymbol *variable, const Expr *value) {
  auto *call = dyn_cast<CallExpr>(value);
  if (call == nullptr || call->hasSpread()) {
    return;
  }
  Builtin builtin = call->builtin();
  if (builtin != Builtin::MathMin && builtin != Builtin::MathMax) {
    return;
  }
  auto &facts = this->state.walk.flow.getFacts();
  auto self = Linear{.term = Term::of(variable), .offset = 0};
  for (const Expr *arg : call->args) {
    auto bound = this->state.flow.measurements.linear(arg);
    if (bound.has_value() && builtin == Builtin::MathMin) {
      this->addAtMost(facts, self, *bound);
    }
    if (bound.has_value() && builtin == Builtin::MathMax) {
      this->addAtMost(facts, *bound, self);
    }
  }
}

void FlowChangesRegistry::assignedLength(const VarSymbol *variable, const Expr *value) {
  const Type *base = variable->type->nonOptional();
  bool isSized = base->isString() || base->isArray() || base->isByteView() ||
                 base->bytes() == Type::Bytes::ArrayBuffer;
  if (!isSized || value->type->isOptional() || value->type->isUndefined()) {
    return;
  }
  auto &facts = this->state.walk.flow.getFacts();
  auto length = this->state.flow.measurements.knownLength(value);
  if (length.has_value() && length->term != Term::lengthOf(variable)) {
    facts.addEqual(Term::lengthOf(variable), length->term, length->offset);
  }
  if (length.has_value()) {
    return;
  }
  auto range = this->state.flow.measurements.lengthRange(value);
  if (range.lo.has_value()) {
    facts.add(Term::zero(), Term::lengthOf(variable), Facts::saturatingSub(1, *range.lo));
  }
  if (range.hi.has_value()) {
    facts.add(Term::lengthOf(variable), Term::zero(), Facts::saturatingAdd(*range.hi, 1));
  }
}

bool FlowChangesRegistry::isShiftSafe(const Term &term, int64_t delta) const {
  const auto &facts = this->state.walk.flow.getFacts();
  if (delta > 0) {
    return facts.canProve(term, Term::zero(), kIntMax - (delta - 1));
  }

  return facts.canProve(Term::zero(), term, Facts::saturatingSub(delta + 1, kIntMin));
}

void FlowChangesRegistry::incremented(const VarSymbol *variable, int64_t delta) {
  if (!variable->type->isInt()) {
    return;
  }
  auto term = Term::of(variable);
  bool isSafe = delta == 0 || this->isShiftSafe(term, delta);
  if (isSafe) {
    this->state.walk.flow.getFacts().shift(term, delta);
  } else {
    this->state.walk.flow.getFacts().forget(variable);
  }
}

void FlowChangesRegistry::calledFunction() {
  auto flow = this->state.walk.flow.takeFlow();
  this->state.flow.changes.forgetCallEffects(flow);
  this->state.walk.flow.replaceFlow(std::move(flow));
}

void FlowChangesRegistry::noteIncremented(const NoteIncremented &task) {
  return this->incremented(task.variable, task.step);
}

bool FlowChangesRegistry::isResizedPart(const Term &term, const Type *array,
                                        const VarSymbol *except) const {
  return term.length && term.var != except && term.var->type->nonOptional() == array;
}

bool FlowChangesRegistry::isResizedTerm(const Term &term, const Type *array,
                                        const VarSymbol *except) const {
  return this->isResizedPart(term.first(), array, except) ||
         this->isResizedPart(term.second(), array, except);
}

bool FlowChangesRegistry::isResizedFact(const Type *array, Resize resize, const VarSymbol *except,
                                        const Facts::Fact &fact) const {
  bool isGrowing = resize == Resize::GROWS || resize == Resize::CHANGES;
  bool isShrinking = resize == Resize::SHRINKS || resize == Resize::CHANGES;
  if (isGrowing && this->isResizedTerm(fact.lhs, array, except)) {
    return true;
  }

  return isShrinking && this->isResizedTerm(fact.rhs, array, except);
}

void FlowChangesRegistry::noteArrayResized(const NoteArrayResized &task) {
  const Type *array = task.array->nonOptional();
  auto isResized =
      std::bind_front(&FlowChangesRegistry::isResizedFact, this, array, task.resize, task.except);
  this->state.walk.flow.getFacts().forgetIf(isResized);
}

void FlowChangesRegistry::rememberAssigned(SourceFile &program) {
  auto nodes = PlacedNodes::collect(program.items, {});
  for (const PlacedExpr &placed : nodes.getExprs()) {
    this->rememberTarget(placed.expr);
  }
}

void FlowChangesRegistry::rememberTarget(const Expr *expr) {
  const Expr *target = this->changedTarget(expr);
  if (target == nullptr) {
    return;
  }
  auto *member = dyn_cast<MemberExpr>(target);
  auto *index = dyn_cast<IndexExpr>(target);
  if (member != nullptr) {
    this->state.flow.assignments.markFieldAssigned(member->name);
  }
  if (index != nullptr) {
    for (const std::string &name : this->keyNames(index->index)) {
      this->state.flow.assignments.markFieldAssigned(name);
    }
  }
}

const Expr *FlowChangesRegistry::changedTarget(const Expr *expr) const {
  auto *assign = dyn_cast<AssignExpr>(expr);
  auto *update = dyn_cast<UpdateExpr>(expr);
  if (assign != nullptr) {
    return assign->target;
  }
  if (update != nullptr) {
    return update->target;
  }

  return nullptr;
}

std::vector<std::string> FlowChangesRegistry::keyNames(const Expr *key) const {
  auto *literal = dyn_cast<StringLitExpr>(key);
  auto *templateExpr = dyn_cast<TemplateExpr>(key);
  auto *number = dyn_cast<IntLitExpr>(key);
  auto *name = dyn_cast<NameExpr>(key);
  auto *member = dyn_cast<MemberExpr>(key);
  if (literal != nullptr) {
    return {ToUtf8::toUtf8(literal->value)};
  }
  if (templateExpr != nullptr && templateExpr->exprs.empty()) {
    return {ToUtf8::toUtf8(templateExpr->quasis[0])};
  }
  if (number != nullptr) {
    return {std::to_string(number->value)};
  }
  if (name != nullptr) {
    return this->state.file.stringConstants.lookup(name->name);
  }
  if (member != nullptr) {
    return this->memberConstants(member);
  }

  return {};
}

std::vector<std::string> FlowChangesRegistry::memberConstants(const MemberExpr *member) const {
  auto *object = dyn_cast<NameExpr>(member->object);
  if (object == nullptr) {
    return {};
  }
  std::string path = (object->name + "." + member->name).str();

  return this->state.file.stringConstants.lookup(path);
}

}
