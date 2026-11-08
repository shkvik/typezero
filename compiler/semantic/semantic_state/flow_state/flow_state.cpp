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

void FlowState::markDefined(const VarSymbol *var) {
  this->flow.defined.insert(var);
}

void FlowState::forgetDefined(const VarSymbol *var) {
  this->flow.defined.erase(var);
}

bool FlowState::isDefined(const VarSymbol *var) const {
  return this->flow.defined.contains(var);
}

void FlowState::addNarrowed(const VarSymbol *var, const Type *type) {
  this->flow.narrowed.push_back({var, type});
}

const Type *FlowState::findNarrowed(const VarSymbol *var) const {
  for (const auto &[narrowed, type] : this->flow.narrowed) {
    if (narrowed == var) {
      return type;
    }
  }

  return nullptr;
}

void FlowState::markOkResult(const VarSymbol *var) {
  this->flow.okResults.insert(var);
}

void FlowState::markErrResult(const VarSymbol *var) {
  this->flow.errResults.insert(var);
}

void FlowState::forgetResultState(const VarSymbol *var) {
  this->flow.okResults.erase(var);
  this->flow.errResults.erase(var);
}

bool FlowState::isOkResult(const VarSymbol *var) const {
  return this->flow.okResults.contains(var);
}

bool FlowState::isErrResult(const VarSymbol *var) const {
  return this->flow.errResults.contains(var);
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
  bool listened = llvm::is_contained(this->flow.errorListened, key);
  bool pending =
      llvm::any_of(this->flow.errorPending, [&](const auto &entry) { return entry.first == key; });
  if (!listened && !pending) {
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

void FlowState::rememberFixedLength(const VarSymbol *var, int64_t length) {
  this->fixedLengths[var] = length;
}

std::optional<int64_t> FlowState::findFixedLength(const VarSymbol *var) const {
  auto found = this->fixedLengths.find(var);
  if (found == this->fixedLengths.end()) {
    return std::nullopt;
  }

  return found->second;
}

}
