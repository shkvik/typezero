#include "compiler/pipeline/check_program/check_state/scope_stack/scope_stack.h"

namespace tz {

void ScopeStack::openScope() {
  this->scopes.emplace_back();
  this->typeScopes.emplace_back();
  this->scopeBranches.push_back(this->branchDepth);
}

void ScopeStack::closeScope() {
  this->scopes.pop_back();
  this->typeScopes.pop_back();
  if (!this->scopeBranches.empty()) {
    this->scopeBranches.pop_back();
  }
}

bool ScopeStack::isTopLevel() const {
  return this->scopes.size() == 1;
}

bool ScopeStack::isTopLevelTypes() const {
  return this->typeScopes.size() == 1;
}

size_t ScopeStack::getScopeDepth() const {
  return this->scopes.size();
}

ScopeEntry ScopeStack::findName(llvm::StringRef name) const {
  for (auto scope = this->scopes.rbegin(); scope != this->scopes.rend(); ++scope) {
    auto found = scope->find(name);
    if (found != scope->end()) {
      return found->second;
    }
  }

  return {};
}

std::optional<ScopeEntry> ScopeStack::findInCurrentScope(llvm::StringRef name) const {
  auto found = this->scopes.back().find(name);
  if (found == this->scopes.back().end()) {
    return std::nullopt;
  }

  return found->second;
}

void ScopeStack::addToCurrentScope(llvm::StringRef name, ScopeEntry entry) {
  this->scopes.back().try_emplace(name, entry);
}

std::optional<size_t> ScopeStack::findDeclaringScope(const VarSymbol *var) const {
  for (size_t i = this->scopes.size(); i-- > 0;) {
    auto found = this->scopes[i].find(var->name);
    if (found != this->scopes[i].end() && found->second.var() == var) {
      return i;
    }
  }

  return std::nullopt;
}

std::vector<llvm::StringRef> ScopeStack::getLocalVarNames() const {
  std::vector<llvm::StringRef> names;
  for (size_t i = 1; i < this->scopes.size(); ++i) {
    for (const auto &entry : this->scopes[i]) {
      if (entry.second.var()) {
        names.push_back(entry.second.var()->name);
      }
    }
  }

  return names;
}

llvm::StringMap<ScopeEntry> ScopeStack::getTopValues() const {
  return this->scopes[0];
}

void ScopeStack::replaceTopValues(llvm::StringMap<ScopeEntry> values) {
  this->scopes[0] = std::move(values);
}

bool ScopeStack::hasTopValue(llvm::StringRef name) const {
  return this->scopes[0].count(name) > 0;
}

std::optional<ScopeEntry> ScopeStack::findTopValue(llvm::StringRef name) const {
  auto found = this->scopes[0].find(name);
  if (found == this->scopes[0].end()) {
    return std::nullopt;
  }

  return found->second;
}

void ScopeStack::addTopValue(llvm::StringRef name, ScopeEntry entry) {
  this->scopes[0].try_emplace(name, entry);
}

void ScopeStack::openTypeScope() {
  this->typeScopes.emplace_back();
}

void ScopeStack::closeTypeScope() {
  this->typeScopes.pop_back();
}

Stmt *ScopeStack::findType(llvm::StringRef name) const {
  for (auto scope = this->typeScopes.rbegin(); scope != this->typeScopes.rend(); ++scope) {
    auto found = scope->find(name);
    if (found != scope->end()) {
      return found->second;
    }
  }

  return nullptr;
}

bool ScopeStack::hasTypeInCurrentScope(llvm::StringRef name) const {
  return this->typeScopes.back().count(name) > 0;
}

Stmt *ScopeStack::findTypeInCurrentScope(llvm::StringRef name) const {
  return this->typeScopes.back().lookup(name);
}

void ScopeStack::addType(llvm::StringRef name, Stmt *decl) {
  this->typeScopes.back().try_emplace(name, decl);
}

void ScopeStack::setType(llvm::StringRef name, Stmt *decl) {
  this->typeScopes.back()[name] = decl;
}

llvm::StringMap<Stmt *> ScopeStack::getTopTypes() const {
  return this->typeScopes[0];
}

void ScopeStack::replaceTopTypes(llvm::StringMap<Stmt *> types) {
  this->typeScopes[0] = std::move(types);
}

bool ScopeStack::hasTopType(llvm::StringRef name) const {
  return this->typeScopes[0].count(name) > 0;
}

Stmt *ScopeStack::findTopType(llvm::StringRef name) const {
  return this->typeScopes[0].lookup(name);
}

void ScopeStack::addTopType(llvm::StringRef name, Stmt *decl) {
  this->typeScopes[0].try_emplace(name, decl);
}

void ScopeStack::enterOnlyTypes(llvm::StringMap<Stmt *> types) {
  this->outerTypeScopes.push_back(std::move(this->typeScopes));
  this->typeScopes.clear();
  this->typeScopes.push_back(std::move(types));
}

void ScopeStack::leaveOnlyTypes() {
  this->typeScopes = std::move(this->outerTypeScopes.back());
  this->outerTypeScopes.pop_back();
}

void ScopeStack::enterBlockNames(BlockNames names) {
  this->blockNames.push_back(std::move(names));
}

void ScopeStack::leaveBlockNames() {
  this->blockNames.pop_back();
}

const BlockNames *ScopeStack::findDeclaredLater(llvm::StringRef name) const {
  for (const BlockNames &block : llvm::reverse(this->blockNames)) {
    if (llvm::is_contained(block.names, name)) {
      return &block;
    }
  }

  return nullptr;
}

void ScopeStack::markUsedInFunction(llvm::StringRef name) {
  this->namesInFunctions.insert(name);
}

bool ScopeStack::isUsedInFunction(llvm::StringRef name) const {
  return this->namesInFunctions.contains(name);
}

void ScopeStack::enterBranch() {
  this->branchDepth += 1;
}

void ScopeStack::leaveBranch() {
  this->branchDepth -= 1;
}

bool ScopeStack::isInsideBranch() const {
  return this->branchDepth != 0;
}

bool ScopeStack::isBranchedSince(size_t scope) const {
  return scope < this->scopeBranches.size() && this->scopeBranches[scope] != this->branchDepth;
}

ScopeSnapshot ScopeStack::saveBody() const {
  ScopeSnapshot saved;
  saved.scopes = this->scopes;
  saved.typeScopes = this->typeScopes;
  saved.blockNames = this->blockNames;
  saved.scopeBranches = this->scopeBranches;
  saved.branchDepth = this->branchDepth;

  return saved;
}

void ScopeStack::keepGroupScopes(const NestedGroup *group) {
  this->scopes.resize(group->scopeDepth);
  this->typeScopes.resize(group->scopeDepth);
  while (!this->blockNames.empty() && this->blockNames.back().functions > group->functionDepth) {
    this->blockNames.pop_back();
  }
}

void ScopeStack::keepTopScopes() {
  this->scopes.resize(1);
  this->typeScopes.resize(1);
  this->blockNames.clear();
}

void ScopeStack::alignBranchesWithScopes() {
  this->scopeBranches.resize(this->scopes.size(), 0);
  this->branchDepth = this->scopeBranches.empty() ? 0 : this->scopeBranches.back();
}

void ScopeStack::restoreBody(ScopeSnapshot saved) {
  this->scopeBranches = std::move(saved.scopeBranches);
  this->branchDepth = saved.branchDepth;
  this->scopes = std::move(saved.scopes);
  this->typeScopes = std::move(saved.typeScopes);
  this->blockNames = std::move(saved.blockNames);
}

}
