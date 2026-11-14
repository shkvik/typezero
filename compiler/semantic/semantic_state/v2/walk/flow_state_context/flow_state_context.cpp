#include "compiler/semantic/semantic_state/v2/walk/flow_state_context/flow_state_context.h"

namespace tz {

const Flow &FlowStateContext::getFlow() const {
  return this->flow;
}

void FlowStateContext::replaceFlow(Flow next) {
  this->flow = std::move(next);
}

Flow FlowStateContext::takeFlow() {
  Flow taken = std::move(this->flow);
  this->flow = Flow();

  return taken;
}

Facts &FlowStateContext::getFacts() {
  return this->flow.facts;
}

void FlowStateContext::markUnreachable() {
  this->flow.unreachable = true;
}

bool FlowStateContext::isUnreachable() const {
  return this->flow.unreachable;
}

void FlowStateContext::markBeforeSuper() {
  this->flow.beforeSuper = true;
}

void FlowStateContext::markSuperCalled() {
  this->flow.beforeSuper = false;
}

bool FlowStateContext::isBeforeSuper() const {
  return this->flow.beforeSuper;
}

void FlowStateContext::addSetField(llvm::StringRef name) {
  if (!llvm::is_contained(this->flow.setFields, name)) {
    this->flow.setFields.push_back(name);
  }
}

void FlowStateContext::forgetSetFields() {
  this->flow.setFields.clear();
}

bool FlowStateContext::isFieldSet(llvm::StringRef name) const {
  return llvm::is_contained(this->flow.setFields, name);
}

void FlowStateContext::markDefined(const VarSymbol *variable) {
  this->flow.defined.insert(variable);
}

void FlowStateContext::forgetDefined(const VarSymbol *variable) {
  this->flow.defined.erase(variable);
}

bool FlowStateContext::isDefined(const VarSymbol *variable) const {
  return this->flow.defined.contains(variable);
}

void FlowStateContext::addNarrowed(const VarSymbol *variable, const Type *type) {
  this->flow.narrowed.push_back({variable, type});
}

const Type *FlowStateContext::findNarrowed(const VarSymbol *variable) const {
  for (const auto &[narrowed, type] : this->flow.narrowed) {
    if (narrowed == variable) {
      return type;
    }
  }

  return nullptr;
}

const Type *FlowStateContext::findVariableType(const VarSymbol *variable) const {
  const Type *narrowed = this->findNarrowed(variable);
  if (narrowed != nullptr) {
    return narrowed;
  }
  if (variable->type->isOptional() && this->flow.defined.contains(variable)) {
    return variable->type->inner();
  }

  return variable->type;
}

void FlowStateContext::markOkResult(const VarSymbol *variable) {
  this->flow.okResults.insert(variable);
}

void FlowStateContext::markErrResult(const VarSymbol *variable) {
  this->flow.errResults.insert(variable);
}

void FlowStateContext::forgetResultState(const VarSymbol *variable) {
  this->flow.okResults.erase(variable);
  this->flow.errResults.erase(variable);
}

bool FlowStateContext::isOkResult(const VarSymbol *variable) const {
  return this->flow.okResults.contains(variable);
}

bool FlowStateContext::isErrResult(const VarSymbol *variable) const {
  return this->flow.errResults.contains(variable);
}

void FlowStateContext::defineField(const FieldPath &path) {
  if (!llvm::is_contained(this->flow.definedFields, path)) {
    this->flow.definedFields.push_back(path);
  }
}

bool FlowStateContext::isFieldDefined(const FieldPath &path) const {
  return llvm::is_contained(this->flow.definedFields, path);
}

void FlowStateContext::forgetFieldsNamed(llvm::StringRef name) {
  llvm::erase_if(this->flow.definedFields,
                 [&](const FieldPath &path) { return llvm::is_contained(path.names, name); });
  llvm::erase_if(this->flow.narrowedFields,
                 [&](const auto &entry) { return llvm::is_contained(entry.first.names, name); });
}

bool FlowStateContext::hasNarrowedFields() const {
  return !this->flow.narrowedFields.empty();
}

const Type *FlowStateContext::findNarrowedField(const FieldPath &path) const {
  for (const auto &[narrowed, type] : this->flow.narrowedFields) {
    if (narrowed == path) {
      return type;
    }
  }

  return nullptr;
}

void FlowStateContext::enterErrorWatch() {
  ErrorWatch outer;
  outer.listened = std::move(this->flow.errorListened);
  outer.pending = std::move(this->flow.errorPending);
  this->outerErrorWatches.push_back(std::move(outer));
  this->flow.errorListened.clear();
  this->flow.errorPending.clear();
}

void FlowStateContext::leaveErrorWatch() {
  ErrorWatch outer = std::move(this->outerErrorWatches.back());
  this->outerErrorWatches.pop_back();
  this->flow.errorListened = std::move(outer.listened);
  this->flow.errorPending = std::move(outer.pending);
}

void FlowStateContext::listenForErrors(const EmitterKey &key) {
  if (!llvm::is_contained(this->flow.errorListened, key)) {
    this->flow.errorListened.push_back(key);
  }
  llvm::erase_if(this->flow.errorPending, [&](const auto &entry) { return entry.first == key; });
}

void FlowStateContext::addPendingErrors(const EmitterKey &key, llvm::SMLoc loc) {
  bool isListened = llvm::is_contained(this->flow.errorListened, key);
  bool isPending =
      llvm::any_of(this->flow.errorPending, [&](const auto &entry) { return entry.first == key; });
  if (!isListened && !isPending) {
    this->flow.errorPending.push_back({key, loc});
  }
}

std::vector<std::pair<EmitterKey, llvm::SMLoc>> FlowStateContext::takePendingErrors() {
  std::vector<std::pair<EmitterKey, llvm::SMLoc>> pending = std::move(this->flow.errorPending);
  this->flow.errorPending.clear();

  return pending;
}

}
