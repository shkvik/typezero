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
#include <functional>
#include <limits>

using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

constexpr int64_t kIntMin = std::numeric_limits<int64_t>::min();
constexpr int64_t kIntMax = std::numeric_limits<int64_t>::max();
constexpr size_t kArrayParam = 2;
constexpr size_t kReduceArrayParam = 3;
}

FlowChecker::FlowChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->byteProofs = &all.byteProofs;
  this->fieldFacts = &all.fieldFacts;
  this->indexProofs = &all.indexProofs;
  this->unionNarrowing = &all.unionNarrowing;
}

void FlowChecker::addAtMost(Facts &facts, const Linear &smaller, const Linear &larger) const {
  facts.add(smaller.term, larger.term, larger.offset - smaller.offset + 1);
}

bool FlowChecker::isArrayLength(const Term &term) const {
  return term.length && term.var->type->nonOptional()->isArray();
}

bool FlowChecker::isChangedByCalls(const VarSymbol *variable) const {
  if (variable->isConst) {
    return false;
  }
  if (variable->isGlobal()) {
    return variable->usedInFunction();
  }
  if (variable->captured()) {
    return true;
  }

  return this->state.getFunctionStack().isAssignedInGroup(variable->name);
}

Flow FlowChecker::join(const Flow &left, const Flow &right) const {
  if (left.unreachable) {
    return right;
  }
  if (right.unreachable) {
    return left;
  }
  Flow result;
  result.facts = left.facts.meet(right.facts);
  this->joinDefined(left, right, result);
  for (const NarrowedVar &narrowed : left.narrowed) {
    auto both = this->narrowedOnBoth(narrowed, right);
    if (both.has_value()) {
      result.narrowed.push_back(*both);
    }
  }
  this->widenWithUndefined(left, right, result);
  this->widenWithUndefined(right, left, result);
  this->joinMarks(left, right, result);
  result.beforeSuper = left.beforeSuper || right.beforeSuper;

  return result;
}

void FlowChecker::joinDefined(const Flow &left, const Flow &right, Flow &result) const {
  for (const VarSymbol *variable : left.defined) {
    if (right.defined.contains(variable)) {
      result.defined.insert(variable);
    }
  }
  for (const FieldPath &path : left.definedFields) {
    if (llvm::is_contained(right.definedFields, path)) {
      result.definedFields.push_back(path);
    }
  }
  for (const auto &narrowed : left.narrowedFields) {
    if (llvm::is_contained(right.narrowedFields, narrowed)) {
      result.narrowedFields.push_back(narrowed);
    }
  }
}

const FlowChecker::NarrowedVar *FlowChecker::narrowedIn(const Flow &flow,
                                                        const VarSymbol *variable) const {
  for (const NarrowedVar &narrowed : flow.narrowed) {
    if (narrowed.first == variable) {
      return &narrowed;
    }
  }

  return nullptr;
}

bool FlowChecker::isPlainUnion(const Type *either, const VarSymbol *variable) const {
  if (either->isError() || !UnionSlots::unionProblem(either).empty()) {
    return false;
  }

  return either != variable->type;
}

std::optional<FlowChecker::NarrowedVar> FlowChecker::narrowedOnBoth(const NarrowedVar &narrowed,
                                                                    const Flow &other) const {
  if (llvm::is_contained(other.narrowed, narrowed)) {
    return narrowed;
  }
  const VarSymbol *variable = narrowed.first;
  const Type *type = narrowed.second;
  const NarrowedVar *otherNarrowed = this->narrowedIn(other, variable);
  bool isGeneric = otherNarrowed == nullptr || variable->type->nonOptional()->isParam() ||
                   type->isParam() || otherNarrowed->second->isParam();
  if (isGeneric) {
    return std::nullopt;
  }
  const auto *either = Type::unionOf({type, otherNarrowed->second});
  if (!this->isPlainUnion(either, variable)) {
    return std::nullopt;
  }
  if (variable->type->isOptional() && either == variable->type->inner()) {
    return std::nullopt;
  }

  return NarrowedVar{variable, either};
}

void FlowChecker::widenWithUndefined(const Flow &empty, const Flow &other, Flow &result) const {
  for (const VarSymbol *variable : empty.undefinedVars) {
    auto widened = this->widenedWithUndefined(variable, other, result);
    if (widened.has_value()) {
      result.narrowed.push_back(*widened);
    }
  }
}

std::optional<FlowChecker::NarrowedVar>
FlowChecker::widenedWithUndefined(const VarSymbol *variable, const Flow &other,
                                  const Flow &result) const {
  const NarrowedVar *narrowed = this->narrowedIn(other, variable);
  const Type *base = variable->type->nonOptional();
  bool isSkipped = narrowed == nullptr || base->isParam() || base->isJson() ||
                   narrowed->second->isParam() || this->narrowedIn(result, variable) != nullptr;
  if (isSkipped) {
    return std::nullopt;
  }
  const auto *either = Type::unionOf({narrowed->second, Type::undefinedTy()});
  if (!this->isPlainUnion(either, variable)) {
    return std::nullopt;
  }

  return NarrowedVar{variable, either};
}

void FlowChecker::joinMarks(const Flow &left, const Flow &right, Flow &result) const {
  for (const VarSymbol *variable : left.undefinedVars) {
    if (right.undefinedVars.contains(variable)) {
      result.undefinedVars.insert(variable);
    }
  }
  for (const VarSymbol *variable : left.okResults) {
    if (right.okResults.contains(variable)) {
      result.okResults.insert(variable);
    }
  }
  for (const VarSymbol *variable : left.errResults) {
    if (right.errResults.contains(variable)) {
      result.errResults.insert(variable);
    }
  }
  for (llvm::StringRef field : left.setFields) {
    if (llvm::is_contained(right.setFields, field)) {
      result.setFields.push_back(field);
    }
  }
  for (const EmitterKey &key : left.errorListened) {
    if (llvm::is_contained(right.errorListened, key)) {
      result.errorListened.push_back(key);
    }
  }
  result.errorPending = left.errorPending;
  for (const auto &pending : right.errorPending) {
    if (!this->hasPending(result.errorPending, pending.first)) {
      result.errorPending.push_back(pending);
    }
  }
}

bool FlowChecker::hasPending(llvm::ArrayRef<std::pair<EmitterKey, llvm::SMLoc>> pending,
                             const EmitterKey &key) const {
  for (const auto &entry : pending) {
    if (entry.first == key) {
      return true;
    }
  }

  return false;
}

void FlowChecker::markDefined(const VarSymbol *variable) {
  this->state.getFlowState().markDefined(variable);
}

void FlowChecker::assigned(const VarSymbol *variable, const Expr *value) {
  const Type *type = variable->type;
  if (type->isError()) {
    return;
  }
  this->resetAssignedFlow(variable, value);
  bool isValueDefined = !value->type->isOptional() && !value->type->isUndefined();
  if (type->isOptional() && isValueDefined) {
    this->state.getFlowState().markDefined(variable);
  }
  if (type->isOptional() && !isValueDefined) {
    this->state.getFlowState().forgetDefined(variable);
  }
  if (type->isResult()) {
    return this->rememberResultState(variable, value->type);
  }
  if (type->isInteger() && !type->isWrapping()) {
    return this->assignedInteger(variable, value);
  }
  this->state.getFlowState().getFacts().forget(variable);

  return this->assignedLength(variable, value);
}

void FlowChecker::resetAssignedFlow(const VarSymbol *variable, const Expr *value) {
  const Type *type = variable->type;
  auto flow = this->state.getFlowState().takeFlow();
  this->fieldFacts->forgetFieldsOf(flow, variable);
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
                     TypeRelations::isImplicitlyConvertible(value->type, base);
  if (isNarrowing) {
    this->unionNarrowing->narrowVariable(flow, variable, value->type);
  }
  this->state.getFlowState().replaceFlow(std::move(flow));
}

void FlowChecker::rememberResultState(const VarSymbol *variable, const Type *given) {
  this->state.getFlowState().forgetResultState(variable);
  if (!given->isResult()) {
    return;
  }
  if (given->errType()->isNever()) {
    this->state.getFlowState().markOkResult(variable);
    return;
  }
  if (given->okType()->isNever()) {
    this->state.getFlowState().markErrResult(variable);
  }
}

void FlowChecker::assignedInteger(const VarSymbol *variable, const Expr *value) {
  auto linear = this->indexProofs->linear(value);
  if (linear.has_value() && linear->term == Term::of(variable)) {
    return this->incremented(variable, linear->offset);
  }
  IntegerRange range = linear.has_value() ? IntegerRange{} : this->byteProofs->valueRange(value);
  auto &facts = this->state.getFlowState().getFacts();
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

void FlowChecker::addMinMaxBounds(const VarSymbol *variable, const Expr *value) {
  auto *call = dyn_cast<CallExpr>(value);
  if (call == nullptr || call->hasSpread()) {
    return;
  }
  Builtin builtin = call->builtin();
  if (builtin != Builtin::MathMin && builtin != Builtin::MathMax) {
    return;
  }
  auto &facts = this->state.getFlowState().getFacts();
  auto self = Linear{.term = Term::of(variable), .offset = 0};
  for (const Expr *arg : call->args) {
    auto bound = this->indexProofs->linear(arg);
    if (bound.has_value() && builtin == Builtin::MathMin) {
      this->addAtMost(facts, self, *bound);
    }
    if (bound.has_value() && builtin == Builtin::MathMax) {
      this->addAtMost(facts, *bound, self);
    }
  }
}

void FlowChecker::assignedLength(const VarSymbol *variable, const Expr *value) {
  const Type *base = variable->type->nonOptional();
  bool isSized = base->isString() || base->isArray() || base->isByteView() ||
                 base->bytes() == Type::Bytes::ArrayBuffer;
  if (!isSized || value->type->isOptional() || value->type->isUndefined()) {
    return;
  }
  auto &facts = this->state.getFlowState().getFacts();
  auto length = this->indexProofs->knownLength(value);
  if (length.has_value() && length->term != Term::lengthOf(variable)) {
    facts.addEqual(Term::lengthOf(variable), length->term, length->offset);
  }
  if (length.has_value()) {
    return;
  }
  auto range = this->indexProofs->lengthRange(value);
  if (range.lo.has_value()) {
    facts.add(Term::zero(), Term::lengthOf(variable), Facts::saturatingSub(1, *range.lo));
  }
  if (range.hi.has_value()) {
    facts.add(Term::lengthOf(variable), Term::zero(), Facts::saturatingAdd(*range.hi, 1));
  }
}

bool FlowChecker::isShiftSafe(const Term &term, int64_t delta) const {
  const auto &facts = this->state.getFlowState().getFacts();
  if (delta > 0) {
    return facts.canProve(term, Term::zero(), kIntMax - (delta - 1));
  }

  return facts.canProve(Term::zero(), term, Facts::saturatingSub(delta + 1, kIntMin));
}

void FlowChecker::incremented(const VarSymbol *variable, int64_t delta) {
  if (!variable->type->isInt()) {
    return;
  }
  auto term = Term::of(variable);
  bool isSafe = delta == 0 || this->isShiftSafe(term, delta);
  if (isSafe) {
    this->state.getFlowState().getFacts().shift(term, delta);
  } else {
    this->state.getFlowState().getFacts().forget(variable);
  }
}

bool FlowChecker::isStayingLocal(const FunctionDecl *functionDecl, llvm::StringRef name) const {
  if (functionDecl == nullptr || functionDecl->body == nullptr) {
    return false;
  }
  auto nodes = PlacedNodes::collect({functionDecl->body}, {});
  size_t uses = this->countUses(nodes, name);
  size_t localUses = this->countLocalUses(nodes, name);

  return localUses == uses;
}

size_t FlowChecker::countUses(const PlacedNodes &nodes, llvm::StringRef name) const {
  size_t uses = 0;
  for (const PlacedExpr &placed : nodes.getExprs()) {
    if (this->isName(placed.expr, name)) {
      uses += 1;
    }
  }

  return uses;
}

size_t FlowChecker::localExprUses(const Expr *expr, llvm::StringRef name) const {
  size_t uses = this->countSpreads(expr, name);
  if (this->isReadingElement(expr, name)) {
    uses += 1;
  }
  if (this->isCallingArrayMethod(expr, name)) {
    uses += 1;
  }

  return uses;
}

size_t FlowChecker::localStmtUses(const Stmt *stmt, llvm::StringRef name) const {
  size_t uses = 0;
  if (this->isIteratingOver(stmt, name)) {
    uses += 1;
  }
  if (this->isReturningName(stmt, name)) {
    uses += 1;
  }

  return uses;
}

size_t FlowChecker::countLocalUses(const PlacedNodes &nodes, llvm::StringRef name) const {
  size_t uses = 0;
  for (const PlacedExpr &placed : nodes.getExprs()) {
    if (!placed.place.isInsideFunction()) {
      uses += this->localExprUses(placed.expr, name);
    }
  }
  for (const PlacedStmt &placed : nodes.getStmts()) {
    if (!placed.place.isInsideFunction()) {
      uses += this->localStmtUses(placed.stmt, name);
    }
  }

  return uses;
}

bool FlowChecker::isName(const Expr *expr, llvm::StringRef name) const {
  auto *named = dyn_cast_or_null<NameExpr>(expr);

  return named != nullptr && named->name == name;
}

bool FlowChecker::isReadingElement(const Expr *expr, llvm::StringRef name) const {
  auto *index = dyn_cast<IndexExpr>(expr);
  auto *member = dyn_cast<MemberExpr>(expr);
  if (index != nullptr) {
    return this->isName(index->object, name);
  }
  if (member != nullptr) {
    bool isLength = BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH);

    return isLength && this->isName(member->object, name);
  }

  return false;
}

size_t FlowChecker::countSpreads(const Expr *expr, llvm::StringRef name) const {
  size_t spreads = 0;
  auto *list = dyn_cast<ArrayLitExpr>(expr);
  auto *call = dyn_cast<CallExpr>(expr);
  if (list != nullptr) {
    for (const ArrayLitExpr::Element &element : list->elements) {
      if (element.spread && this->isName(element.expr, name)) {
        spreads += 1;
      }
    }
  }
  if (call != nullptr) {
    for (size_t index = 0; index < call->args.size(); ++index) {
      if (call->spreads(index) && this->isName(call->args[index], name)) {
        spreads += 1;
      }
    }
  }

  return spreads;
}

bool FlowChecker::isCallingArrayMethod(const Expr *expr, llvm::StringRef name) const {
  auto *call = dyn_cast<CallExpr>(expr);
  if (call == nullptr) {
    return false;
  }
  auto *callee = dyn_cast<MemberExpr>(call->callee);
  if (callee == nullptr || !this->isName(callee->object, name)) {
    return false;
  }
  const auto *method = ArrayMethodTable::find(callee->name);
  if (method == nullptr) {
    return false;
  }
  const CallbackArrayMethod *callback = method->form.as<CallbackArrayMethod>();
  bool isSorting = method->form.is<SortingArrayMethod>();
  if (callback == nullptr && !isSorting) {
    return true;
  }
  bool isReducing = callback != nullptr && callback->callback == ArrayCallback::REDUCE;
  size_t arrayParam = isReducing ? kReduceArrayParam : kArrayParam;

  return this->isArrayHiddenFromCallbacks(call, arrayParam);
}

bool FlowChecker::isArrayHiddenFromCallbacks(const CallExpr *call, size_t arrayParam) const {
  for (size_t index = 0; index < call->args.size(); ++index) {
    auto *callback = dyn_cast<FunctionExpr>(call->args[index]);
    if (callback == nullptr && index == 0) {
      return false;
    }
    if (callback != nullptr && callback->fn->params.size() > arrayParam) {
      return false;
    }
  }

  return true;
}

bool FlowChecker::isIteratingOver(const Stmt *stmt, llvm::StringRef name) const {
  auto *loop = dyn_cast<ForOfStmt>(stmt);

  return loop != nullptr && this->isName(loop->iterable, name);
}

bool FlowChecker::isReturningName(const Stmt *stmt, llvm::StringRef name) const {
  auto *returnStmt = dyn_cast<ReturnStmt>(stmt);
  if (returnStmt == nullptr) {
    return false;
  }
  const auto *value = this->unwrapResult(returnStmt->value);

  return this->isName(value, name);
}

const Expr *FlowChecker::unwrapResult(const Expr *value) const {
  auto *wrap = dyn_cast_or_null<CallExpr>(value);
  if (wrap == nullptr || wrap->args.size() != 1) {
    return value;
  }
  auto *maker = dyn_cast<NameExpr>(wrap->callee);
  if (maker == nullptr) {
    return value;
  }
  auto global = GlobalNameTable::find(maker->name);
  bool isWrapping = global == GlobalName::OK || global == GlobalName::ERR;

  return isWrapping ? wrap->args[0] : value;
}

bool FlowChecker::isCallChangedPart(const Term &term) const {
  if (term.var == nullptr) {
    return false;
  }
  bool isSharedLength = this->isArrayLength(term) && !term.var->unshared();

  return isSharedLength || this->isChangedByCalls(term.var);
}

bool FlowChecker::isCallChangedTerm(const Term &term) const {
  return this->isCallChangedPart(term.first()) || this->isCallChangedPart(term.second());
}

bool FlowChecker::isCallChangedFact(const Facts::Fact &fact) const {
  return this->isCallChangedTerm(fact.lhs) || this->isCallChangedTerm(fact.rhs);
}

bool FlowChecker::isCallChangedPath(const FieldPath &path) const {
  if (this->isChangedByCalls(path.var)) {
    return true;
  }
  for (llvm::StringRef field : path.names) {
    if (this->state.getFlowState().isFieldAssigned(field)) {
      return true;
    }
  }

  return false;
}

void FlowChecker::forgetCallEffects(Flow &flow) const {
  flow.facts.forgetIf(std::bind_front(&FlowChecker::isCallChangedFact, this));
  this->forgetCallChanged(flow.defined);
  this->forgetCallChanged(flow.undefinedVars);
  this->forgetCallChanged(flow.okResults);
  this->forgetCallChanged(flow.errResults);
  auto narrowed = std::vector<NarrowedVar>();
  for (const NarrowedVar &entry : flow.narrowed) {
    if (!this->isChangedByCalls(entry.first)) {
      narrowed.push_back(entry);
    }
  }
  flow.narrowed = std::move(narrowed);
  auto definedFields = std::vector<FieldPath>();
  for (const FieldPath &path : flow.definedFields) {
    if (!this->isCallChangedPath(path)) {
      definedFields.push_back(path);
    }
  }
  flow.definedFields = std::move(definedFields);
  auto narrowedFields = decltype(flow.narrowedFields)();
  for (const auto &entry : flow.narrowedFields) {
    if (!this->isCallChangedPath(entry.first)) {
      narrowedFields.push_back(entry);
    }
  }
  flow.narrowedFields = std::move(narrowedFields);
}

void FlowChecker::forgetCallChanged(llvm::SmallPtrSetImpl<const VarSymbol *> &variables) const {
  for (const VarSymbol *variable : llvm::to_vector(variables)) {
    if (this->isChangedByCalls(variable)) {
      variables.erase(variable);
    }
  }
}

void FlowChecker::calledFunction() {
  auto flow = this->state.getFlowState().takeFlow();
  this->forgetCallEffects(flow);
  this->state.getFlowState().replaceFlow(std::move(flow));
}

bool FlowChecker::isResizedPart(const Term &term, const Type *array,
                                const VarSymbol *except) const {
  return term.length && term.var != except && term.var->type->nonOptional() == array;
}

bool FlowChecker::isResizedTerm(const Term &term, const Type *array,
                                const VarSymbol *except) const {
  return this->isResizedPart(term.first(), array, except) ||
         this->isResizedPart(term.second(), array, except);
}

bool FlowChecker::isResizedFact(const Type *array, Resize resize, const VarSymbol *except,
                                const Facts::Fact &fact) const {
  bool isGrowing = resize == Resize::GROWS || resize == Resize::CHANGES;
  bool isShrinking = resize == Resize::SHRINKS || resize == Resize::CHANGES;
  if (isGrowing && this->isResizedTerm(fact.lhs, array, except)) {
    return true;
  }

  return isShrinking && this->isResizedTerm(fact.rhs, array, except);
}

void FlowChecker::arrayResized(const Type *type, Resize resize, const VarSymbol *except) {
  const Type *array = type->nonOptional();
  auto isResized = std::bind_front(&FlowChecker::isResizedFact, this, array, resize, except);
  this->state.getFlowState().getFacts().forgetIf(isResized);
}

}
