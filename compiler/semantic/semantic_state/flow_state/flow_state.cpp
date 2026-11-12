#include "compiler/semantic/semantic_state/flow_state/flow_state.h"

namespace tz {

const Flow &FlowState::getFlow() const {
  return this->flow;
}

void FlowState::replaceFlow(Flow next) {
  this->flow = std::move(next);
}

Flow FlowState::takeFlow() {
  Flow taken = std::move(this->flow);
  this->flow = Flow();

  return taken;
}

Facts &FlowState::getFacts() {
  return this->flow.facts;
}

void FlowState::markUnreachable() {
  this->flow.unreachable = true;
}

bool FlowState::isUnreachable() const {
  return this->flow.unreachable;
}

void FlowState::markBeforeSuper() {
  this->flow.beforeSuper = true;
}

void FlowState::markSuperCalled() {
  this->flow.beforeSuper = false;
}

bool FlowState::isBeforeSuper() const {
  return this->flow.beforeSuper;
}

void FlowState::addSetField(llvm::StringRef name) {
  if (!llvm::is_contained(this->flow.setFields, name)) {
    this->flow.setFields.push_back(name);
  }
}

void FlowState::forgetSetFields() {
  this->flow.setFields.clear();
}

bool FlowState::isFieldSet(llvm::StringRef name) const {
  return llvm::is_contained(this->flow.setFields, name);
}

void FlowState::markDefined(const VarSymbol *variable) {
  this->flow.defined.insert(variable);
}

void FlowState::forgetDefined(const VarSymbol *variable) {
  this->flow.defined.erase(variable);
}

bool FlowState::isDefined(const VarSymbol *variable) const {
  return this->flow.defined.contains(variable);
}

void FlowState::addNarrowed(const VarSymbol *variable, const Type *type) {
  this->flow.narrowed.push_back({variable, type});
}

const Type *FlowState::findNarrowed(const VarSymbol *variable) const {
  for (const auto &[narrowed, type] : this->flow.narrowed) {
    if (narrowed == variable) {
      return type;
    }
  }

  return nullptr;
}

void FlowState::markOkResult(const VarSymbol *variable) {
  this->flow.okResults.insert(variable);
}

void FlowState::markErrResult(const VarSymbol *variable) {
  this->flow.errResults.insert(variable);
}

void FlowState::forgetResultState(const VarSymbol *variable) {
  this->flow.okResults.erase(variable);
  this->flow.errResults.erase(variable);
}

bool FlowState::isOkResult(const VarSymbol *variable) const {
  return this->flow.okResults.contains(variable);
}

bool FlowState::isErrResult(const VarSymbol *variable) const {
  return this->flow.errResults.contains(variable);
}

void FlowState::defineField(const FieldPath &path) {
  if (!llvm::is_contained(this->flow.definedFields, path)) {
    this->flow.definedFields.push_back(path);
  }
}

bool FlowState::isFieldDefined(const FieldPath &path) const {
  return llvm::is_contained(this->flow.definedFields, path);
}

void FlowState::forgetFieldsNamed(llvm::StringRef name) {
  llvm::erase_if(this->flow.definedFields,
                 [&](const FieldPath &path) { return llvm::is_contained(path.names, name); });
  llvm::erase_if(this->flow.narrowedFields,
                 [&](const auto &entry) { return llvm::is_contained(entry.first.names, name); });
}

bool FlowState::hasNarrowedFields() const {
  return !this->flow.narrowedFields.empty();
}

const Type *FlowState::findNarrowedField(const FieldPath &path) const {
  for (const auto &[narrowed, type] : this->flow.narrowedFields) {
    if (narrowed == path) {
      return type;
    }
  }

  return nullptr;
}

void FlowState::enterErrorWatch() {
  ErrorWatch outer;
  outer.listened = std::move(this->flow.errorListened);
  outer.pending = std::move(this->flow.errorPending);
  this->outerErrorWatches.push_back(std::move(outer));
  this->flow.errorListened.clear();
  this->flow.errorPending.clear();
}

void FlowState::leaveErrorWatch() {
  ErrorWatch outer = std::move(this->outerErrorWatches.back());
  this->outerErrorWatches.pop_back();
  this->flow.errorListened = std::move(outer.listened);
  this->flow.errorPending = std::move(outer.pending);
}

void FlowState::listenForErrors(const EmitterKey &key) {
  if (!llvm::is_contained(this->flow.errorListened, key)) {
    this->flow.errorListened.push_back(key);
  }
  llvm::erase_if(this->flow.errorPending, [&](const auto &entry) { return entry.first == key; });
}

void FlowState::addPendingErrors(const EmitterKey &key, llvm::SMLoc loc) {
  bool isListened = llvm::is_contained(this->flow.errorListened, key);
  bool isPending =
      llvm::any_of(this->flow.errorPending, [&](const auto &entry) { return entry.first == key; });
  if (!isListened && !isPending) {
    this->flow.errorPending.push_back({key, loc});
  }
}

std::vector<std::pair<EmitterKey, llvm::SMLoc>> FlowState::takePendingErrors() {
  std::vector<std::pair<EmitterKey, llvm::SMLoc>> pending = std::move(this->flow.errorPending);
  this->flow.errorPending.clear();

  return pending;
}

void FlowState::markFieldAssigned(llvm::StringRef name) {
  this->assignedFields.insert(name);
}

bool FlowState::isFieldAssigned(llvm::StringRef name) const {
  return this->assignedFields.contains(name);
}

void FlowState::rememberFixedLength(const VarSymbol *variable, int64_t length) {
  this->fixedLengths[variable] = length;
}

std::optional<int64_t> FlowState::findFixedLength(const VarSymbol *variable) const {
  auto found = this->fixedLengths.find(variable);
  if (found == this->fixedLengths.end()) {
    return std::nullopt;
  }

  return found->second;
}

}
