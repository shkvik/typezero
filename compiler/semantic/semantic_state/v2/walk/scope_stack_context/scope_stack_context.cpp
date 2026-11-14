#include "compiler/semantic/semantic_state/v2/walk/scope_stack_context/scope_stack_context.h"

namespace tz {

void ScopeStackContext::openScope() {
  this->scopes.emplace_back();
  this->typeScopes.emplace_back();
  this->scopeBranches.push_back(this->branchDepth);
}

void ScopeStackContext::closeScope() {
  this->scopes.pop_back();
  this->typeScopes.pop_back();
  if (!this->scopeBranches.empty()) {
    this->scopeBranches.pop_back();
  }
}

bool ScopeStackContext::isTopLevel() const {
  return this->scopes.size() == 1;
}

bool ScopeStackContext::isTopLevelTypes() const {
  return this->typeScopes.size() == 1;
}

size_t ScopeStackContext::getScopeDepth() const {
  return this->scopes.size();
}

ScopeEntry ScopeStackContext::findName(llvm::StringRef name) const {
  for (auto scope = this->scopes.rbegin(); scope != this->scopes.rend(); ++scope) {
    auto found = scope->find(name);
    if (found != scope->end()) {
      return found->second;
    }
  }

  return {};
}

std::optional<ScopeEntry> ScopeStackContext::findInCurrentScope(llvm::StringRef name) const {
  auto found = this->scopes.back().find(name);
  if (found == this->scopes.back().end()) {
    return std::nullopt;
  }

  return found->second;
}

void ScopeStackContext::addToCurrentScope(llvm::StringRef name, ScopeEntry entry) {
  this->scopes.back().try_emplace(name, entry);
}

std::optional<size_t> ScopeStackContext::findDeclaringScope(const VarSymbol *variable) const {
  for (size_t index = this->scopes.size(); index-- > 0;) {
    auto found = this->scopes[index].find(variable->name);
    if (found != this->scopes[index].end() && found->second.var() == variable) {
      return index;
    }
  }

  return std::nullopt;
}

std::vector<llvm::StringRef> ScopeStackContext::getLocalVarNames() const {
  auto names = std::vector<llvm::StringRef>();
  for (size_t index = 1; index < this->scopes.size(); ++index) {
    for (const auto &entry : this->scopes[index]) {
      if (entry.second.var()) {
        names.push_back(entry.second.var()->name);
      }
    }
  }

  return names;
}

llvm::StringMap<ScopeEntry> ScopeStackContext::getTopValues() const {
  return this->scopes[0];
}

void ScopeStackContext::replaceTopValues(llvm::StringMap<ScopeEntry> values) {
  this->scopes[0] = std::move(values);
}

bool ScopeStackContext::hasTopValue(llvm::StringRef name) const {
  return this->scopes[0].count(name) > 0;
}

std::optional<ScopeEntry> ScopeStackContext::findTopValue(llvm::StringRef name) const {
  auto found = this->scopes[0].find(name);
  if (found == this->scopes[0].end()) {
    return std::nullopt;
  }

  return found->second;
}

void ScopeStackContext::addTopValue(llvm::StringRef name, ScopeEntry entry) {
  this->scopes[0].try_emplace(name, entry);
}

void ScopeStackContext::openTypeScope() {
  this->typeScopes.emplace_back();
}

void ScopeStackContext::closeTypeScope() {
  this->typeScopes.pop_back();
}

Stmt *ScopeStackContext::findType(llvm::StringRef name) const {
  for (auto scope = this->typeScopes.rbegin(); scope != this->typeScopes.rend(); ++scope) {
    auto found = scope->find(name);
    if (found != scope->end()) {
      return found->second;
    }
  }

  return nullptr;
}

bool ScopeStackContext::hasTypeInCurrentScope(llvm::StringRef name) const {
  return this->typeScopes.back().count(name) > 0;
}

Stmt *ScopeStackContext::findTypeInCurrentScope(llvm::StringRef name) const {
  return this->typeScopes.back().lookup(name);
}

void ScopeStackContext::addType(llvm::StringRef name, Stmt *decl) {
  this->typeScopes.back().try_emplace(name, decl);
}

void ScopeStackContext::setType(llvm::StringRef name, Stmt *decl) {
  this->typeScopes.back()[name] = decl;
}

llvm::StringMap<Stmt *> ScopeStackContext::getTopTypes() const {
  return this->typeScopes[0];
}

void ScopeStackContext::replaceTopTypes(llvm::StringMap<Stmt *> types) {
  this->typeScopes[0] = std::move(types);
}

bool ScopeStackContext::hasTopType(llvm::StringRef name) const {
  return this->typeScopes[0].count(name) > 0;
}

Stmt *ScopeStackContext::findTopType(llvm::StringRef name) const {
  return this->typeScopes[0].lookup(name);
}

void ScopeStackContext::addTopType(llvm::StringRef name, Stmt *decl) {
  this->typeScopes[0].try_emplace(name, decl);
}

void ScopeStackContext::enterOnlyTypes(llvm::StringMap<Stmt *> types) {
  this->outerTypeScopes.push_back(std::move(this->typeScopes));
  this->typeScopes.clear();
  this->typeScopes.push_back(std::move(types));
}

void ScopeStackContext::leaveOnlyTypes() {
  this->typeScopes = std::move(this->outerTypeScopes.back());
  this->outerTypeScopes.pop_back();
}

void ScopeStackContext::enterBlockNames(BlockNames names) {
  this->blockNames.push_back(std::move(names));
}

void ScopeStackContext::leaveBlockNames() {
  this->blockNames.pop_back();
}

const BlockNames *ScopeStackContext::findDeclaredLater(llvm::StringRef name) const {
  for (const BlockNames &block : llvm::reverse(this->blockNames)) {
    if (llvm::is_contained(block.names, name)) {
      return &block;
    }
  }

  return nullptr;
}

void ScopeStackContext::enterBranch() {
  this->branchDepth += 1;
}

void ScopeStackContext::leaveBranch() {
  this->branchDepth -= 1;
}

bool ScopeStackContext::isInsideBranch() const {
  return this->branchDepth != 0;
}

bool ScopeStackContext::isBranchedSince(size_t scope) const {
  return scope < this->scopeBranches.size() && this->scopeBranches[scope] != this->branchDepth;
}

ScopeSnapshot ScopeStackContext::saveBody() const {
  ScopeSnapshot saved;
  saved.scopes = this->scopes;
  saved.typeScopes = this->typeScopes;
  saved.blockNames = this->blockNames;
  saved.scopeBranches = this->scopeBranches;
  saved.branchDepth = this->branchDepth;

  return saved;
}

void ScopeStackContext::keepGroupScopes(const NestedGroup *group) {
  this->scopes.resize(group->scopeDepth);
  this->typeScopes.resize(group->scopeDepth);
  while (!this->blockNames.empty() && this->blockNames.back().functions > group->functionDepth) {
    this->blockNames.pop_back();
  }
}

void ScopeStackContext::keepTopScopes() {
  this->scopes.resize(1);
  this->typeScopes.resize(1);
  this->blockNames.clear();
}

void ScopeStackContext::alignBranchesWithScopes() {
  this->scopeBranches.resize(this->scopes.size(), 0);
  this->branchDepth = this->scopeBranches.empty() ? 0 : this->scopeBranches.back();
}

void ScopeStackContext::restoreBody(ScopeSnapshot saved) {
  this->scopeBranches = std::move(saved.scopeBranches);
  this->branchDepth = saved.branchDepth;
  this->scopes = std::move(saved.scopes);
  this->typeScopes = std::move(saved.typeScopes);
  this->blockNames = std::move(saved.blockNames);
}

}
