#include "compiler/semantic/flow_v2/changes/flow_changes.h"
#include "compiler/program/library/array/array_method_table/array_method_table.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>

using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

constexpr size_t kArrayParam = 2;
constexpr size_t kReduceArrayParam = 3;

}

FlowChanges::FlowChanges(SemanticState &owner)
    : state(owner) {}

bool FlowChanges::isArrayLength(const Term &term) const {
  return term.length && term.var->type->nonOptional()->isArray();
}

bool FlowChanges::isChangedByCalls(const VarSymbol *variable) const {
  if (variable->isConst) {
    return false;
  }
  if (variable->isGlobal()) {
    return variable->usedInFunction();
  }
  if (variable->captured()) {
    return true;
  }

  return this->state.walk.functions.isAssignedInGroup(variable->name);
}

bool FlowChanges::isStayingLocal(const FunctionDecl *functionDecl, llvm::StringRef name) const {
  if (functionDecl == nullptr || functionDecl->body == nullptr) {
    return false;
  }
  auto nodes = PlacedNodes::collect({functionDecl->body}, {});
  size_t uses = this->countUses(nodes, name);
  size_t localUses = this->countLocalUses(nodes, name);

  return localUses == uses;
}

size_t FlowChanges::countUses(const PlacedNodes &nodes, llvm::StringRef name) const {
  size_t uses = 0;
  for (const PlacedExpr &placed : nodes.getExprs()) {
    if (this->isName(placed.expr, name)) {
      uses += 1;
    }
  }

  return uses;
}

size_t FlowChanges::localExprUses(const Expr *expr, llvm::StringRef name) const {
  size_t uses = this->countSpreads(expr, name);
  if (this->isReadingElement(expr, name)) {
    uses += 1;
  }
  if (this->isCallingArrayMethod(expr, name)) {
    uses += 1;
  }

  return uses;
}

size_t FlowChanges::localStmtUses(const Stmt *stmt, llvm::StringRef name) const {
  size_t uses = 0;
  if (this->isIteratingOver(stmt, name)) {
    uses += 1;
  }
  if (this->isReturningName(stmt, name)) {
    uses += 1;
  }

  return uses;
}

size_t FlowChanges::countLocalUses(const PlacedNodes &nodes, llvm::StringRef name) const {
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

bool FlowChanges::isName(const Expr *expr, llvm::StringRef name) const {
  auto *named = dyn_cast_or_null<NameExpr>(expr);

  return named != nullptr && named->name == name;
}

bool FlowChanges::isReadingElement(const Expr *expr, llvm::StringRef name) const {
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

size_t FlowChanges::countSpreads(const Expr *expr, llvm::StringRef name) const {
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

bool FlowChanges::isCallingArrayMethod(const Expr *expr, llvm::StringRef name) const {
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

bool FlowChanges::isArrayHiddenFromCallbacks(const CallExpr *call, size_t arrayParam) const {
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

bool FlowChanges::isIteratingOver(const Stmt *stmt, llvm::StringRef name) const {
  auto *loop = dyn_cast<ForOfStmt>(stmt);

  return loop != nullptr && this->isName(loop->iterable, name);
}

bool FlowChanges::isReturningName(const Stmt *stmt, llvm::StringRef name) const {
  auto *returnStmt = dyn_cast<ReturnStmt>(stmt);
  if (returnStmt == nullptr) {
    return false;
  }
  const auto *value = this->unwrapResult(returnStmt->value);

  return this->isName(value, name);
}

const Expr *FlowChanges::unwrapResult(const Expr *value) const {
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

bool FlowChanges::isCallChangedPart(const Term &term) const {
  if (term.var == nullptr) {
    return false;
  }
  bool isSharedLength = this->isArrayLength(term) && !term.var->unshared();

  return isSharedLength || this->isChangedByCalls(term.var);
}

bool FlowChanges::isCallChangedTerm(const Term &term) const {
  return this->isCallChangedPart(term.first()) || this->isCallChangedPart(term.second());
}

bool FlowChanges::isCallChangedFact(const Facts::Fact &fact) const {
  return this->isCallChangedTerm(fact.lhs) || this->isCallChangedTerm(fact.rhs);
}

bool FlowChanges::isCallChangedPath(const FieldPath &path) const {
  if (this->isChangedByCalls(path.var)) {
    return true;
  }
  for (llvm::StringRef field : path.names) {
    if (this->state.flow.assignments.isFieldAssigned(field)) {
      return true;
    }
  }

  return false;
}

void FlowChanges::forgetCallEffects(Flow &flow) const {
  flow.facts.forgetIf(std::bind_front(&FlowChanges::isCallChangedFact, this));
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

void FlowChanges::forgetCallChanged(llvm::SmallPtrSetImpl<const VarSymbol *> &variables) const {
  for (const VarSymbol *variable : llvm::to_vector(variables)) {
    if (this->isChangedByCalls(variable)) {
      variables.erase(variable);
    }
  }
}

bool FlowChanges::isPathOf(const VarSymbol *variable, const FieldPath &path) const {
  return path.var == variable;
}

bool FlowChanges::isNarrowedPathOf(const VarSymbol *variable,
                                   const std::pair<FieldPath, const Type *> &entry) const {
  return entry.first.var == variable;
}

void FlowChanges::forgetFieldsOf(Flow &flow, const VarSymbol *variable) const {
  auto isOwned = std::bind_front(&FlowChanges::isPathOf, this, variable);
  auto isNarrowedOwned = std::bind_front(&FlowChanges::isNarrowedPathOf, this, variable);
  llvm::erase_if(flow.definedFields, isOwned);
  llvm::erase_if(flow.narrowedFields, isNarrowedOwned);
}

}
